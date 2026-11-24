// jsp_snapshot_hostthr.h -- ENHANCEMENT (rollback latency, test11 freeze tail): the host threads the snapshot freeze leaves running because they
// belong to the GPU driver or the graphics runtime. Pure (no OS headers): jsp_snapshot.cpp feeds it the module of a thread's start address and the
// thread's description, and keeps the skip list; installer/tools/snapshot_hostthr_test.cpp drives the same code.
//
// Why: in the laptop's online matches (test10 bundles a and d) 21 freezes took 29-158 ms, each held by one thread: an NVIDIA driver thread (start
// nvwgf2umx.dll+0xecd8f0) or a "D3D Background Thread", both at priority -15 (idle). GetThreadContext returns only once the target is really suspended,
// and an idle-priority thread on a busy CPU waits a long time for the core it needs to take the suspension.
// Why it is safe to leave them running: guest memory (the vm_base reservation) is written only by the guest's PPU / SPU threads, the toolkit's and the
// port's own host threads (all created by code in the exe, though many start in the CRT's or winpthreads' trampoline), and the RSX side (the FIFO walker on the frame clock thread, reported to the RSX write
// guard by patch 7917). The D3D12 backend never gives guest memory to D3D (no OpenExistingHeapFromAddress, no placed resource over it): uploads copy
// guest memory into D3D's own buffers on the backend's threads, readbacks copy out of them on the backend's threads, and it registers no D3D callback
// (it polls the info queue). So a driver thread only ever touches the driver's own memory. The frozen-interval write check (JSP_SNAPSHOT_FROZEN_WRITES,
// on by default) still watches every saved guest page during each freeze and logs "[snap] FROZEN-WRITES" if anything the freeze left running wrote one.
// JSP_SNAPSHOT_SKIP_DRIVER=0 freezes them as before.
#pragma once
#include <cstdint>

namespace jsh {

enum Cls : uint8_t {
    Unknown = 0,
    Other = 1,    // frozen: start address in the exe, or nothing named it a driver thread after kNameChecks reads
    Driver = 2,   // left running
    Maybe = 3,    // started elsewhere (the CRT's or winpthreads' trampoline, ntdll's pool): frozen; its description may still name it (read again a few times)
};

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
inline bool iprefix(const char* s, const char* p)
{
    if (!s) return false;
    for (; *p; s++, p++) if (!*s || lower(*s) != lower(*p)) return false;
    return true;
}

// Module base names (no folder) of the GPU user-mode drivers and the D3D / DXGI runtime, matched as case-insensitive prefixes.
// NVIDIA nvwgf2umx.dll / nvwgf2um.dll (D3D10-12 UMD), nvldumdx.dll (loader), nvgpucomp64.dll (shader compiler);
// AMD amdxc64.dll / amdxx64.dll (D3D12), atidxx64.dll / atiumd64.dll; Intel igd12umd64.dll, igd10iumd64.dll, igc64.dll (shader compiler);
// Microsoft d3d12.dll / d3d12core.dll / d3d12sdklayers.dll, d3d11.dll, dxgi.dll, dxcore.dll, d3dscache.dll (the shader cache service).
inline bool driver_module(const char* base)
{
    static const char* const k[] = {"nvwgf2um", "nvldumd", "nvgpucomp", "amdxc", "amdxx", "atidxx", "atiumd", "igd12um", "igd10i", "igc64",
                                    "d3d12", "d3d11", "dxgi", "dxcore", "d3dscache", nullptr};
    for (int i = 0; k[i]; i++) if (iprefix(base, k[i])) return true;
    return false;
}

// A thread description (UTF-8) that names a graphics runtime thread started through the CRT (start ucrtbase.dll, so the start module says nothing).
// "D3D Background Thread N" (laptop, RTX 4060: priority -15, 51 ms to stop). The toolkit and the port never give a thread such a name.
inline bool driver_name(const char* name) { return iprefix(name, "D3D Background Thread"); }

// The class from the start address: a start in the exe is frozen for good, a start in a driver module is left running for good, the rest is Maybe.
// Most of the exe's own threads are Maybe (_beginthreadex / std::thread start in ucrtbase or libwinpthread): they stay frozen because no such
// thread is named "D3D Background Thread", and after kNameChecks reads they are Other.
inline Cls from_start(const char* module_base, bool in_exe)
{
    if (in_exe) return Other;
    if (module_base && driver_module(module_base)) return Driver;
    return Maybe;
}

// Per registry slot: who the slot held when it was classified (tid, handle, creation time) and what it is.
struct Ent { uint32_t tid = 0; const void* h = nullptr; uint64_t ctime = 0; Cls cls = Unknown; uint8_t name_checks = 0; bool logged = false; };
constexpr uint8_t kNameChecks = 8;   // a Maybe thread's description is read at most this often (first pass, then every kNamePeriod passes), then it is Other
constexpr uint32_t kNamePeriod = 16;
inline bool want_name_check(const Ent& e, uint32_t pass) { return e.cls == Maybe && e.name_checks < kNameChecks && (e.name_checks == 0 || pass % kNamePeriod == 0); }
inline void name_checked(Ent& e, bool is_driver)
{
    e.name_checks++;
    if (is_driver) e.cls = Driver;
    else if (e.name_checks >= kNameChecks) e.cls = Other;
}

// The threads a freeze skips: filled outside the freeze, read inside it (fixed storage, nothing allocates). A thread is skipped only when its tid, its
// handle and its creation time all match the entry, so a slot or handle reused by another thread since the classification is frozen as usual.
struct Skip { uint32_t tid = 0; const void* h = nullptr; uint64_t ctime = 0; };
constexpr int kSkip = 64;
struct SkipList {
    Skip e[kSkip]; int n = 0; uint32_t overflow = 0;
    void clear() { n = 0; }
    bool add(uint32_t tid, const void* h, uint64_t ctime)
    {
        if (!ctime) return false;                  // no creation time read: never skipped
        if (n >= kSkip) { overflow++; return false; }   // full: frozen as before
        e[n++] = Skip{tid, h, ctime};
        return true;
    }
    const Skip* find(uint32_t tid, const void* h) const
    {
        for (int i = 0; i < n; i++) if (e[i].tid == tid && e[i].h == h) return &e[i];
        return nullptr;
    }
    // ctime_now: the creation time read from the handle at the freeze (0 = the read failed: frozen)
    bool skip(uint32_t tid, const void* h, uint64_t ctime_now) const
    {
        const Skip* s = find(tid, h);
        return s && ctime_now && s->ctime == ctime_now;
    }
};

}  // namespace jsh
