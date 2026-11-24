// jsp_sound_preload.cpp -- FIX (online fight hitches, test8 bundles): keep the game's sound archive in RAM in a rollback session.
//
// Why: in a rollback match the game waits at every tick boundary until the sound library's file threads ("CRI FS ...") and its SPU decode jobs are
// idle (fs_wait in jsp_net_game.cpp, for determinism). Each voice or music read that has to go to the disk stalls that tick: on a test8 laptop
// (7.7 GB RAM, the game's working set 2 GB plus 2.7 GB in the page file) the wait was 2.2-2.7 ms a boundary in both matches, up to 70 ms, and it was
// the largest boundary part of 559 hitches; the host PC's own wait dropped from 2.4 ms in its first fight to 0.2-0.3 ms in later ones (the same
// file, now in the Windows file cache). The barrier stays as it is; this makes the reads it waits for more likely to come from RAM.
//
// How: the game opens the sound archive (partition_op_sound_ps3.cpk) once at boot and keeps it open. jsp_sound_preload_install() (rollback sessions
// only, from jsp_net_game_install, before the guest starts) chains a cellFs virtual-file provider (toolkit patch 6502, ps3emu/fs_vfile.h) in front
// of the one already there (the mods folder's, or none). It never serves a file itself: it hands every open to the previous provider and only
// notes the host path of the sound archive (the first open, or the hdd0 install copy if that opens within kPickMs). A background thread then reads
// the file chunk by chunk with ReadFile into a scratch buffer (a disk error is a return code there, never an exception), and touches the same
// chunk in a read-only mapped view of the file, so the pages also sit in this process's working set; the game's own reads (fread on its own
// handle) are served from the same pages by the Windows cache. The bytes the game reads, its read calls and their order do not change: only how
// long a read takes. jsp_sound_preload_fight_start() (each rollback session start) checks how much is still in the working set, re-checks the RAM
// cap, and reads only the chunks that dropped out again, paced, at the lowest thread priority and low I/O priority.
//
// Size: JSP_SOUND_PRELOAD_MB (default 512, 0 = off), capped by the file (381 MB), by half the free RAM above 1 GB and on a PC under 12 GB by a
// quarter of its RAM (jsp_sound_preload_rule.h). A file-backed view adds no commit charge; under memory pressure Windows drops its pages like any
// cached file (read again from the disk). Everything here lives for the whole process (no destructors run under a detached thread at exit).
// Log lines: "[sound-preload] ...". POSIX branch (mmap, pread) only for installer/tools/sound_preload_test.sh.
#include "jsp_sound_preload_rule.h"
#include "ps3emu/fs_vfile.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr uint64_t kChunk = 8ull << 20;
constexpr uint64_t kPage = 4096;

const ps3_fs_vfile_ops* g_prev = nullptr;   // the provider that was there before (the mods folder's), or none
std::atomic<int> g_noted{0};                // a sound archive open was seen (the preload thread is started)
std::atomic<int> g_state{0};                // 0 not started, 1 reading, 2 held, -1 skipped or failed
std::atomic<int> g_refreshing{0};
std::atomic<bool> g_lock{false};            // guards g_host / g_host_hdd0 / g_picked (a spin lock: trivially destructible)
char g_host[1024];                          // the copy to preload (plain array: nothing to destroy at exit)
bool g_host_hdd0 = false, g_picked = false;
const unsigned char* g_view = nullptr;      // the mapped view (kept for the life of the process), g_len bytes
uint64_t g_len = 0, g_file_size = 0;
long long g_budget_mb = 0;
#ifdef _WIN32
HANDLE g_file = INVALID_HANDLE_VALUE;       // our own read handle (kept), used by one thread at a time
#else
int g_file = -1;
#endif

void lock() { while (g_lock.exchange(true, std::memory_order_acquire)) std::this_thread::yield(); }
void unlock() { g_lock.store(false, std::memory_order_release); }
double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double mb(uint64_t b) { return (double)b / 1048576.0; }
const char* base_name(const char* p) { const char* b = p; for (; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1; return b; }

void ram(uint64_t* avail, uint64_t* total)
{
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) { *avail = (uint64_t)ms.ullAvailPhys; *total = (uint64_t)ms.ullTotalPhys; } else *avail = *total = 0;
#else
    const long a = sysconf(_SC_AVPHYS_PAGES), t = sysconf(_SC_PHYS_PAGES), sz = sysconf(_SC_PAGESIZE);
    *avail = a > 0 && sz > 0 ? (uint64_t)a * (uint64_t)sz : 0;
    *total = t > 0 && sz > 0 ? (uint64_t)t * (uint64_t)sz : 0;
#endif
}

// Read [off, off + n) of the file into scratch with our own handle (a disk error is a return value), then touch the same pages of the view: they
// are in the cache now, so the touch only maps them into the working set. False = a read error.
bool warm_chunk(uint64_t off, uint64_t n, unsigned char* scratch)
{
#ifdef _WIN32
    OVERLAPPED o = {};
    o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(g_file, scratch, (DWORD)n, &got, &o) || got != (DWORD)n) return false;
#else
    if (pread(g_file, scratch, (size_t)n, (off_t)off) != (ssize_t)n) return false;
#endif
    volatile unsigned sink = 0;
    for (uint64_t p = 0; p < n; p += kPage) sink += g_view[off + p];
    (void)sink;
    return true;
}

// Bytes of [off, off + n) of the view in the working set now (POSIX: in RAM). UINT64_MAX = cannot tell. n <= kChunk.
uint64_t resident_range(uint64_t off, uint64_t n)
{
    const uint64_t pages = (n + kPage - 1) / kPage;
#ifdef _WIN32
    typedef struct { PVOID va; ULONG_PTR attr; } Info;   // PSAPI_WORKING_SET_EX_INFORMATION; attr bit 0 = Valid (in the working set)
    typedef BOOL (WINAPI *Query)(HANDLE, PVOID, DWORD);
    static const Query q = (Query)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    static Info buf[kChunk / kPage];   // one caller at a time (the preload thread, then refreshes one after another)
    if (!q) return UINT64_MAX;
    for (uint64_t k = 0; k < pages; ++k) { buf[k].va = (PVOID)(g_view + off + k * kPage); buf[k].attr = 0; }
    if (!q(GetCurrentProcess(), buf, (DWORD)(pages * sizeof(Info)))) return UINT64_MAX;
    uint64_t in = 0;
    for (uint64_t k = 0; k < pages; ++k) if (buf[k].attr & 1) in += kPage;
#else
    static unsigned char v[kChunk / kPage];
    if (mincore((void*)(g_view + off), (size_t)n, v) != 0) return UINT64_MAX;
    uint64_t in = 0;
    for (uint64_t k = 0; k < pages; ++k) if (v[k] & 1) in += kPage;
#endif
    return in > n ? n : in;
}

void skip(const char* name, const char* why)
{
    std::fprintf(stderr, "[sound-preload] %s: not preloaded (%s)\n", name, why);
    g_state.store(-1);
}

void close_file()
{
#ifdef _WIN32
    CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE;
#else
    close(g_file); g_file = -1;
#endif
}

void preload()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(jspre::kPickMs));   // the game opens all its archives at boot: the hdd0 copy wins if it comes
    char host[sizeof g_host];
    lock(); std::memcpy(host, g_host, sizeof host); g_picked = true; unlock();
    const char* name = base_name(host);
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const double t0 = now_ms();
    g_file = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return skip(name, "cannot open it");
    { FILE_IO_PRIORITY_HINT_INFO hint; hint.PriorityHint = IoPriorityHintLow;   // our reads queue behind the game's (this handle only)
      SetFileInformationByHandle(g_file, FileIoPriorityHintInfo, &hint, sizeof hint); }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(g_file, &sz) || sz.QuadPart <= 0) { close_file(); return skip(name, "cannot read its size"); }
    g_file_size = (uint64_t)sz.QuadPart;
#else
    const double t0 = now_ms();
    g_file = open(host, O_RDONLY);
    if (g_file < 0) return skip(name, "cannot open it");
    struct stat st;
    if (fstat(g_file, &st) != 0 || st.st_size <= 0) { close_file(); return skip(name, "cannot read its size"); }
    g_file_size = (uint64_t)st.st_size;
#endif
    uint64_t avail = 0, total = 0;
    ram(&avail, &total);
    const uint64_t len = jspre::preload_bytes(g_file_size, g_budget_mb, avail, total);
    if (!len) {
        char why[160];
        std::snprintf(why, sizeof why, "free RAM %.0f MB of %.0f MB leaves no room above the 1024 MB reserve", mb(avail), mb(total));
        close_file();
        return skip(name, why);
    }
#ifdef _WIN32
    HANDLE m = CreateFileMappingA(g_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) { close_file(); return skip(name, "cannot map it"); }
    g_view = (const unsigned char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, (SIZE_T)len);
    CloseHandle(m);   // the view keeps the mapping
    if (!g_view) { close_file(); return skip(name, "cannot map a view of it"); }
#else
    void* v = mmap(nullptr, (size_t)len, PROT_READ, MAP_SHARED, g_file, 0);
    if (v == MAP_FAILED) { close_file(); return skip(name, "cannot map it"); }
    g_view = (const unsigned char*)v;
#endif
    unsigned char* scratch = new unsigned char[kChunk];
    uint64_t done = 0;
    for (; done < len; done += kChunk) {
        const uint64_t n = len - done < kChunk ? len - done : kChunk;
        if (!warm_chunk(done, n, scratch)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // leave the disk to the game's own reads between chunks
    }
    delete[] scratch;
    const double ms = now_ms() - t0;
    if (done < len) {
        std::fprintf(stderr, "[sound-preload] %s: read error at %.1f MB: stopped, the first %.1f MB are kept\n", name, mb(done), mb(done));
        if (!done) { g_state.store(-1); return; }
        g_len = done;
    } else g_len = len;
    std::fprintf(stderr, "[sound-preload] %s: %.1f of %.1f MB read into RAM in %.0f ms (JSP_SOUND_PRELOAD_MB %lld; free RAM at the start %.0f of %.0f MB)\n",
                 name, mb(g_len), mb(g_file_size), ms, g_budget_mb, mb(avail), mb(total));
    g_state.store(2);
}

ps3_fs_vfile* p_open(const char* guest_path, const char* host_path)
{
    if (host_path && jspre::is_sound_archive(guest_path) && std::strlen(host_path) < sizeof g_host) {
        bool start = false;
        lock();
        if (!g_picked && jspre::better_copy(g_host[0] != 0, g_host_hdd0, guest_path)) {
            std::memcpy(g_host, host_path, std::strlen(host_path) + 1);
            g_host_hdd0 = jspre::is_hdd0(guest_path);
        }
        start = !g_noted.exchange(1);
        unlock();
        if (start) { g_state.store(1); std::thread(preload).detach(); }
    }
    return g_prev ? g_prev->open(guest_path, host_path) : nullptr;
}
// Only a view the previous provider made reaches these (p_open never claims a file itself).
uint64_t p_size(ps3_fs_vfile* vf) { return g_prev->size(vf); }
size_t p_read(ps3_fs_vfile* vf, FILE* host, uint64_t pos, void* dst, size_t n) { return g_prev->read(vf, host, pos, dst, n); }
void p_close(ps3_fs_vfile* vf) { g_prev->close(vf); }
const ps3_fs_vfile_ops k_ops = { p_open, p_size, p_read, p_close };

void refresh()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    const double t0 = now_ms();
    uint64_t in = 0;
    bool known = true;
    for (uint64_t off = 0; off < g_len; off += kChunk) {
        const uint64_t r = resident_range(off, g_len - off < kChunk ? g_len - off : kChunk);
        if (r == UINT64_MAX) { known = false; break; }
        in += r;
    }
    uint64_t avail = 0, total = 0;
    ram(&avail, &total);
    // The pages still in the working set are ours already: they count as room. A smaller cap leaves the tail alone (Windows drops it as needed).
    uint64_t limit = jspre::preload_bytes(g_file_size, g_budget_mb, avail + (known ? in : 0), total);
    if (limit > g_len) limit = g_len;
    unsigned char* scratch = new unsigned char[kChunk];
    uint64_t reread = 0;
    bool err = false;
    for (uint64_t off = 0; off < limit; off += kChunk) {
        const uint64_t n = limit - off < kChunk ? limit - off : kChunk;
        if (resident_range(off, n) == n) continue;   // already in the working set
        if (!warm_chunk(off, n, scratch)) { err = true; break; }
        reread += n;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));   // paced: the fight's own sound reads go first
    }
    delete[] scratch;
    const double ms = now_ms() - t0;
    if (known) std::fprintf(stderr, "[sound-preload] fight start: %.1f of %.1f MB still in the working set; %.1f MB read again in %.0f ms (cap now %.1f MB, free RAM %.0f of %.0f MB)%s\n",
                            mb(in), mb(g_len), mb(reread), ms, mb(limit), mb(avail), mb(total), err ? "; stopped at a read error" : "");
    else std::fprintf(stderr, "[sound-preload] fight start: %.1f MB read again in %.0f ms (cap now %.1f MB, the working set cannot be queried)%s\n",
                      mb(reread), ms, mb(limit), err ? "; stopped at a read error" : "");
    g_refreshing.store(0);
}

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

// Each rollback session start: how much of the archive is still in the working set; the chunks that dropped out are read again in the background.
extern "C" void jsp_sound_preload_fight_start(void)
{
    const int s = g_state.load();
    if (s == 1) { std::fprintf(stderr, "[sound-preload] fight start: the first preload is still reading\n"); return; }
    if (s != 2 || g_refreshing.exchange(1)) return;
    std::thread(refresh).detach();
}

// For the host test: 0 not started, 1 reading, 2 held, -1 skipped or failed; *bytes = the preloaded length.
extern "C" int jsp_sound_preload_state(uint64_t* bytes)
{
    const int s = g_state.load();
    if (bytes) *bytes = s == 2 ? g_len : 0;
    return s;
}
// For the host test: 1 while a fight-start refresh runs.
extern "C" int jsp_sound_preload_refreshing(void) { return g_refreshing.load(); }
