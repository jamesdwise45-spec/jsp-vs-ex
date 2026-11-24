// jsp_sound_preload.cpp -- FIX (online fight hitches, test8 bundles): keep the game's sound archive in RAM in a rollback session.
//
// Why: in a rollback match the game waits at every tick boundary until the sound library's file threads ("CRI FS ...") and its SPU decode jobs are
// idle (fs_wait in jsp_net_game.cpp, for determinism). Each voice or music read that has to go to the disk stalls that tick: on a test8 laptop
// (7.7 GB RAM, the game's working set 2 GB plus 2.7 GB in the page file) the wait was 2.2-2.7 ms a boundary in both matches, up to 70 ms, and it was
// the largest boundary part of 559 hitches; the host PC's own wait dropped from 2.4 ms in its first fight to 0.2-0.3 ms in later ones (the same
// file, now in the Windows file cache). The barrier stays as it is; this makes the reads it waits for come from RAM.
//
// How: the game opens the sound archive (partition_op_sound_ps3.cpk) once at boot and keeps it open. jsp_sound_preload_install() (rollback sessions
// only, from jsp_net_game_install, before the guest starts) chains a cellFs virtual-file provider (toolkit patch 6502, ps3emu/fs_vfile.h) in front
// of the one already there (the mods folder's, or none). It never serves a file itself: it hands every open to the previous provider and only
// notes the host path of the first open of the sound archive. A background thread then maps that file read-only and touches every page of it, so
// the pages sit in this process's working set; the game's own reads (fread on its own handle) are served from the same pages by the Windows cache.
// The bytes the game reads, its read calls and their order do not change: only how long a read takes. jsp_sound_preload_fight_start() (each
// rollback session start) checks how much is still in RAM, logs it, and touches the pages again in the background.
//
// Size: JSP_SOUND_PRELOAD_MB (default 512, 0 = off), capped by the file (381 MB) and by half the free RAM above 1 GB (jsp_sound_preload_rule.h).
// A file-backed view adds no commit charge; under memory pressure Windows drops its pages like any cached file (read again from the disk).
// Log lines: "[sound-preload] ...". POSIX branch (mmap) only for installer/tools/sound_preload_test.sh.
#include "jsp_sound_preload_rule.h"
#include "ps3emu/fs_vfile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

const ps3_fs_vfile_ops* g_prev = nullptr;   // the provider that was there before (the mods folder's), or none
std::atomic<int> g_noted{0};                // the first open of the sound archive was seen
std::atomic<int> g_state{0};                // 0 not started, 1 reading, 2 held, -1 skipped or failed
std::atomic<int> g_refreshing{0};
std::string g_host;                         // written before g_state leaves 0
const unsigned char* g_view = nullptr;      // the mapped view (kept for the life of the process), g_len bytes
uint64_t g_len = 0, g_file_size = 0;
long long g_budget_mb = 0;

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double mb(uint64_t b) { return (double)b / 1048576.0; }
const char* base_name(const std::string& p) { const size_t k = p.find_last_of("/\\"); return p.c_str() + (k == std::string::npos ? 0 : k + 1); }

uint64_t avail_phys()
{
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullAvailPhys : 0;
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES), sz = sysconf(_SC_PAGESIZE);
    return pages > 0 && sz > 0 ? (uint64_t)pages * (uint64_t)sz : 0;
#endif
}

// Touch every page of [0, g_len): the first pass reads them from the disk, a later one only finds the pages that were dropped.
void touch_all(bool pace)
{
    const uint64_t kChunk = 8ull << 20;
    volatile unsigned sink = 0;
#ifdef _WIN32
    typedef BOOL (WINAPI *Prefetch)(HANDLE, ULONG_PTR, PVOID, ULONG);   // PrefetchVirtualMemory: Windows 8 and later (looked up, so Windows 7 still starts)
    static const Prefetch prefetch = (Prefetch)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "PrefetchVirtualMemory");
#endif
    for (uint64_t off = 0; off < g_len; off += kChunk) {
        const uint64_t n = g_len - off < kChunk ? g_len - off : kChunk;
#ifdef _WIN32
        if (prefetch) { struct { PVOID a; SIZE_T n; } r = { (PVOID)(g_view + off), (SIZE_T)n }; prefetch(GetCurrentProcess(), 1, &r, 0); }
#else
        madvise((void*)(g_view + off), (size_t)n, MADV_WILLNEED);   // off is a multiple of kChunk: page aligned
#endif
        for (uint64_t p = 0; p < n; p += 4096) sink += g_view[off + p];
        if (pace) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // leave the disk to the game's own reads between chunks
    }
    (void)sink;
}

// Pages of the view that are in RAM now (Windows: in this process's working set), in bytes. UINT64_MAX = cannot tell.
uint64_t resident_bytes()
{
#ifdef _WIN32
    typedef struct { PVOID va; ULONG_PTR attr; } Info;   // PSAPI_WORKING_SET_EX_INFORMATION; attr bit 0 = Valid (in the working set)
    typedef BOOL (WINAPI *Query)(HANDLE, PVOID, DWORD);
    static const Query q = (Query)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    if (!q) return UINT64_MAX;
    static Info buf[4096];
    uint64_t in = 0;
    for (uint64_t off = 0; off < g_len; off += 4096ull * 4096) {
        DWORD k = 0;
        for (; k < 4096 && off + (uint64_t)k * 4096 < g_len; ++k) { buf[k].va = (PVOID)(g_view + off + (uint64_t)k * 4096); buf[k].attr = 0; }
        if (!q(GetCurrentProcess(), buf, k * (DWORD)sizeof(Info))) return UINT64_MAX;
        for (DWORD i = 0; i < k; ++i) if (buf[i].attr & 1) in += 4096;
    }
    return in > g_len ? g_len : in;
#else
    const size_t pages = (size_t)((g_len + 4095) / 4096);
    std::string v(pages, '\0');
    if (mincore((void*)g_view, (size_t)g_len, (unsigned char*)&v[0]) != 0) return UINT64_MAX;
    uint64_t in = 0;
    for (char c : v) if (c & 1) in += 4096;
    return in > g_len ? g_len : in;
#endif
}

void skip(const char* why)
{
    std::fprintf(stderr, "[sound-preload] %s: not preloaded (%s)\n", base_name(g_host), why);
    g_state.store(-1);
}

void preload()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const double t0 = now_ms();
    HANDLE f = CreateFileA(g_host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return skip("cannot open it");
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) { CloseHandle(f); return skip("cannot read its size"); }
    g_file_size = (uint64_t)sz.QuadPart;
#else
    const double t0 = now_ms();
    const int f = open(g_host.c_str(), O_RDONLY);
    if (f < 0) return skip("cannot open it");
    struct stat st;
    if (fstat(f, &st) != 0 || st.st_size <= 0) { close(f); return skip("cannot read its size"); }
    g_file_size = (uint64_t)st.st_size;
#endif
    const uint64_t free_ram = avail_phys();
    g_len = jspre::preload_bytes(g_file_size, g_budget_mb, free_ram);
    if (!g_len) {
        char why[160];
        std::snprintf(why, sizeof why, "free RAM %.0f MB leaves no room above the 1024 MB reserve", mb(free_ram));
#ifdef _WIN32
        CloseHandle(f);
#else
        close(f);
#endif
        return skip(why);
    }
#ifdef _WIN32
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);   // the mapping keeps the file open
    if (!m) return skip("cannot map it");
    g_view = (const unsigned char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, (SIZE_T)g_len);
    CloseHandle(m);   // the view keeps the mapping
    if (!g_view) return skip("cannot map a view of it");
#else
    void* v = mmap(nullptr, (size_t)g_len, PROT_READ, MAP_SHARED, f, 0);
    close(f);
    if (v == MAP_FAILED) return skip("cannot map it");
    g_view = (const unsigned char*)v;
#endif
    touch_all(true);
    const double ms = now_ms() - t0;
    std::fprintf(stderr, "[sound-preload] %s: %.1f of %.1f MB held in RAM in %.0f ms (JSP_SOUND_PRELOAD_MB %lld, free RAM at the start %.0f MB)\n",
                 base_name(g_host), mb(g_len), mb(g_file_size), ms, g_budget_mb, mb(free_ram));
    g_state.store(2);
}

ps3_fs_vfile* p_open(const char* guest_path, const char* host_path)
{
    if (host_path && jspre::is_sound_archive(guest_path) && !g_noted.exchange(1)) {
        g_host = host_path;
        g_state.store(1);
        std::thread(preload).detach();
    }
    return g_prev ? g_prev->open(guest_path, host_path) : nullptr;
}
// Only a view the previous provider made reaches these (p_open never claims a file itself).
uint64_t p_size(ps3_fs_vfile* vf) { return g_prev->size(vf); }
size_t p_read(ps3_fs_vfile* vf, FILE* host, uint64_t pos, void* dst, size_t n) { return g_prev->read(vf, host, pos, dst, n); }
void p_close(ps3_fs_vfile* vf) { g_prev->close(vf); }
const ps3_fs_vfile_ops k_ops = { p_open, p_size, p_read, p_close };

}  // namespace

// Before the guest starts (it opens the archive at boot). Once per process; JSP_SOUND_PRELOAD_MB=0 installs nothing.
extern "C" void jsp_sound_preload_install(void)
{
    static std::atomic<int> once{0};
    if (once.exchange(1)) return;
    g_budget_mb = jspre::parse_budget_mb(std::getenv("JSP_SOUND_PRELOAD_MB"));
    if (g_budget_mb <= 0) { std::fprintf(stderr, "[sound-preload] off (JSP_SOUND_PRELOAD_MB=0)\n"); return; }
    g_prev = __atomic_load_n(&g_ps3_fs_vfile_ops, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_ps3_fs_vfile_ops, &k_ops, __ATOMIC_RELEASE);
    std::fprintf(stderr, "[sound-preload] on: up to %lld MB of %s once the game opens it%s\n", g_budget_mb, jspre::kArchive,
                 g_prev ? " (in front of the mods folder's provider)" : "");
}

// Each rollback session start: how much of the archive is still in RAM; the dropped pages are read again in the background.
extern "C" void jsp_sound_preload_fight_start(void)
{
    const int s = g_state.load();
    if (s == 1) { std::fprintf(stderr, "[sound-preload] fight start: the first preload is still reading\n"); return; }
    if (s != 2 || g_refreshing.exchange(1)) return;
    std::thread([] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        const double t0 = now_ms();
        const uint64_t in = resident_bytes();
        touch_all(false);
        if (in == UINT64_MAX) std::fprintf(stderr, "[sound-preload] fight start: %.1f MB touched again in %.0f ms\n", mb(g_len), now_ms() - t0);
        else std::fprintf(stderr, "[sound-preload] fight start: %.1f of %.1f MB were still in RAM, the rest read again in %.0f ms\n", mb(in), mb(g_len), now_ms() - t0);
        g_refreshing.store(0);
    }).detach();
}

// For the host test: 0 not started, 1 reading, 2 held, -1 skipped or failed; *bytes = the preloaded length.
extern "C" int jsp_sound_preload_state(uint64_t* bytes)
{
    const int s = g_state.load();
    if (bytes) *bytes = s == 2 ? g_len : 0;
    return s;
}
