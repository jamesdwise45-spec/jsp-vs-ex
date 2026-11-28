// jsp_rb_guard.h -- FIX (test13, rollback guards): the pure rules behind four guards of jsp_net_game.cpp, host-tested (installer/tools/rb_guard_test.sh).
// No OS, runtime or game headers.
//
//   1. NULL-call loop (JSP_NET_NULLCALL_GUARD): a guest thread that makes kNullCallsPerTick NULL indirect calls between two tick boundaries is looping on garbage
//      (room 34: a job worker called through a rewound job queue forever, the toolkit's guard returns from every such call, and the commit gate then waited its 15 s
//      budget on a worker that never parks). NcThread is one thread's counter (thread-local in the port); nc_count says when the count reaches the limit, once per epoch.
//   2. Desync first: when one batch of session events holds a Desync and a Disconnected (the PC that found the split leaves right after it), the Desync is handled
//      first, so the log and the report name it (room 38: the PC that split saw only "Connection lost").
//   3. Helper-thread register write-back: why one was skipped (the log line of every skip).
//   4. Loader boundary wait (JSP_NET_LOADER_WAIT, off by default): the bounded wait for the file loader thread at its idle site, with an off switch for the match after
//      kLoaderMissesOff timeouts in a row (a loader that never goes idle at a boundary must not cost every tick its timeout).
#pragma once
#include <algorithm>
#include <cstdint>

namespace jrg {

// ---- 1. NULL-call loop ------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kNullCallsPerTick = 32;   // a healthy match makes none (12 field logs: NULL calls only after the room-34 rollback)
struct NcThread { uint32_t epoch = 0xFFFFFFFFu, n = 0; };
// One NULL call by this thread during boundary epoch `epoch`. True exactly once per epoch: on the call that reaches `limit` (the caller raises its flag then).
inline bool nc_count(NcThread& t, uint32_t epoch, uint32_t limit = kNullCallsPerTick)
{
    if (t.epoch != epoch) { t.epoch = epoch; t.n = 0; }
    return ++t.n == limit;
}

// ---- 2. Desync first --------------------------------------------------------------------------------------------------------------------------------------------
// Moves the events `is_desync` names to the front, keeping the order inside both groups (a batch is handled front to back and ends at the first ending event).
template<class V, class F> void desync_first(V& events, F is_desync)
{
    std::stable_partition(events.begin(), events.end(), is_desync);
}

// ---- 3. Register write-back skips -------------------------------------------------------------------------------------------------------------------------------
enum class RegSkip { None, NoSave, Ended, Moved };
// have_save: the ring holds the target tick's register save; the rest describe one thread saved there: its HLE marker kind and caller lr at the save, and now.
inline RegSkip regs_skip_kind(bool have_save, bool alive_now, uint32_t kind_was, uint32_t lr_was, uint32_t kind_now, uint32_t lr_now)
{
    if (!have_save) return RegSkip::NoSave;
    if (!alive_now) return RegSkip::Ended;
    if (kind_now != kind_was || lr_now != lr_was) return RegSkip::Moved;
    return RegSkip::None;
}
inline const char* regs_skip_name(RegSkip s)
{
    switch (s) {
    case RegSkip::NoSave: return "no save of that tick";
    case RegSkip::Ended: return "thread ended";
    case RegSkip::Moved: return "not parked in the same wait";
    default: return "written back";
    }
}

// ---- 4. Loader boundary wait ------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kLoaderIdleLr = 0x0052E658u;   // sp_file_server_load_thread's idle wait (room 34: where it sat at a clean save)
constexpr double kLoaderWaitMs = 100.0;           // per boundary
constexpr int kLoaderMissesOff = 3;               // timeouts in a row that turn the wait off for the rest of the match
inline bool loader_idle(bool alive, uint32_t kind, uint32_t lr) { return !alive || (kind != 0 && lr == kLoaderIdleLr); }   // an ended loader waits for nothing
struct LoaderGate {
    int misses = 0; bool off = false; uint64_t waits = 0, timeouts = 0;
    // The outcome of one boundary's wait (idle = the loader was seen idle on two samples in a row inside the bound). True when this outcome turned the wait off.
    bool result(bool idle)
    {
        waits++;
        if (idle) { misses = 0; return false; }
        timeouts++;
        if (off || ++misses < kLoaderMissesOff) return false;
        off = true; return true;
    }
};

}  // namespace jrg
