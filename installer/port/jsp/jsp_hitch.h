// jsp_hitch.h -- DIAGNOSTIC (per-hitch line, always on in a rollback match): test6 field logs (two PCs, 100-178 ms ping) had 11-18 "[perf] hitches"
// (presented-frame gaps over 50 ms, p50 76 ms) per match on each PC, also in a match with 0 rollbacks, and each one is an audio gap. The match summary only
// counts them; nothing in the log says what the main thread did during one. This file builds ONE line per such gap (the first kMaxLines of a match) with
// where the time went, and a cause word; the summary gets a per-cause count. Pure (no game, no Windows): jsp_net_game.cpp feeds it on the main thread,
// jsp_stepper.cpp fills the frame clock part (Clock) on the frame clock thread, installer/tools/hitch_test.cpp tests it.
//
// A gap runs from one presented frame's boundary to the next (as jpperf::on_frame measures it). It holds:
//   main thread  the boundaries in it, split into session (GekkoNet update + the snapshot save or restore it calls; the snapshot part separately, from
//                jsp_snapshot.cpp's running sum), hold (waiting for a tick), audio (audio lock), sound (file-thread barrier and sound server cycle) and
//                other; and the guest time between boundaries (game code, its HLE calls, its waits for the flip and the vblanks). The guest time after
//                an iteration that ran a re-run tick is "replay".
//   frame clock  (lockstep clock, jsp_stepper.cpp) how late the guest's flip became ready against its 30 Hz deadline, how late it was taken, the longest
//                FIFO drain call (draws, texture uploads, a pipeline built on the drawing thread), the longest stretch outside the clock pass
//                (main.cpp's present, drain and window pump), and once per late flip a sample of where the main thread was (boundary stage, or the
//                guest's HLE call and call site).
// Cost: a few QueryPerformanceCounter reads per boundary and per clock pass, one thread census per flip that is 8 ms late, one line per hitch.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace jph {

inline constexpr double kHitchMs = 50.0;       // the same threshold as jpperf::kHitchMs
inline constexpr uint32_t kMaxLines = 60;      // lines per match; later hitches are only counted
inline constexpr double kGuestNominalMs = 22.0;   // guest time of an ordinary tick (33.3 ms minus a ~11 ms boundary): only the excess competes as a cause
inline constexpr double kLateMs = 8.0;         // a flip this late (ready after its deadline) counts as a late guest; also when the clock samples the main thread
inline constexpr double kRenderMs = 20.0;      // a FIFO drain or present/pump stretch this long counts as the frame clock's own delay

enum Part { Session, Snapshot, Hold, Audio, Files, Server, Other, Guest, Replay, kParts };   // Snapshot is inside Session

// The frame clock's view of one gap (jsp_stepper.cpp fills it; jsp_stepper_hitch_take hands it over and starts the next one).
struct Clock {
    double ready_late_ms = -1e9;   // the latest a flip became ready after its deadline (negative: early; -1e9: no flip became ready)
    double take_late_ms = -1e9;    // the latest a flip was taken after its deadline
    double drain_max_ms = 0;       // longest single cellGcm_rsx_process_fifo call in the lockstep clock
    double outside_max_ms = 0;     // longest stretch between two lockstep passes (main.cpp: present, drain, window pump)
    uint32_t flips = 0, restarts = 0;
    int sampled = 0;               // 1 = the main thread was sampled at a flip kLateMs late
    int stage = 0;                 // g_jsp_main_stage at the sample (0 guest code, 1.. boundary stages)
    uint32_t kind = 0, id = 0, lr = 0;   // the main guest thread at the sample: HLE call kind (0 = guest code), its id, the call site
    double sample_late_ms = 0;     // how late the flip was when sampled
};

struct Gap {
    double part[kParts] = {};
    double snap_gate = 0;          // of the snapshot time: the gate (waiting for the game's threads to park, then the freeze)
    uint32_t boundaries = 0, snap_ops = 0, reruns = 0, rollbacks = 0, depth_max = 0;
};

enum Cause { CSnapshot, CSession, CHold, CAudio, CSound, COther, CReplay, CGuestLate, CDrain, CPresent, CGuest, kCauses };
inline const char* cause_name(int c)
{
    static const char* n[kCauses] = { "snapshot", "session", "hold-for-tick", "audio-lock", "sound-barrier", "boundary-other", "rollback-replay",
                                      "guest-late", "fifo-drain", "present-pump", "guest-flip-wait" };
    return c >= 0 && c < kCauses ? n[c] : "?";
}
inline const char* stage_name(int s)   // the same names as jsp_stepper.cpp's main_stage_name (g_jsp_main_stage)
{
    switch (s) { case 0: return "guest-code"; case 1: return "boundary:session"; case 2: return "boundary:hold-for-a-tick"; case 3: return "boundary:audio-lock";
                 case 4: return "boundary:sound-server"; case 5: return "boundary:other"; }
    return "?";
}

// The part that took the most time. Boundary parts count whole; the guest only above kGuestNominalMs (an ordinary tick spends that much there). A guest
// winner is split with the frame clock: its flip ready late = the guest was late (the sample names where); ready on time but a drain or a present/pump
// stretch over kRenderMs = the frame clock's own work (draws, uploads, a pipeline built on the drawing thread); else the guest waited for the flip.
inline int cause(const Gap& g, const Clock& c)
{
    struct { int c; double ms; } v[] = {
        { CSnapshot, g.part[Snapshot] }, { CSession, g.part[Session] - g.part[Snapshot] }, { CHold, g.part[Hold] }, { CAudio, g.part[Audio] },
        { CSound, g.part[Files] + g.part[Server] }, { COther, g.part[Other] }, { CReplay, g.part[Replay] }, { CGuest, g.part[Guest] - kGuestNominalMs } };
    int best = 0;
    for (int i = 1; i < (int)(sizeof v / sizeof v[0]); i++) if (v[i].ms > v[best].ms) best = i;
    if (v[best].c != CGuest) return v[best].c;
    if (c.ready_late_ms >= kLateMs) return CGuestLate;
    if (c.drain_max_ms >= kRenderMs && c.drain_max_ms >= c.outside_max_ms) return CDrain;
    if (c.outside_max_ms >= kRenderMs) return CPresent;
    return CGuest;
}

struct Match {
    Gap gap;
    uint32_t hitches = 0, lines = 0;
    uint32_t by_cause[kCauses] = {};
};

// Main thread, per boundary that the gap covers.
inline void add_boundary(Gap& g, double session, double snapshot, double snap_gate, uint32_t snap_ops, double hold, double audio, double files, double server, double total)
{
    g.boundaries++;
    g.part[Session] += session; g.part[Snapshot] += snapshot; g.snap_gate += snap_gate; g.snap_ops += snap_ops; g.part[Hold] += hold; g.part[Audio] += audio;
    g.part[Files] += files; g.part[Server] += server;
    const double other = total - session - hold - audio - files - server;
    if (other > 0) g.part[Other] += other;
}
// The guest stretch between two boundaries; after an iteration that ran a re-run tick it is replay time.
inline void add_guest(Gap& g, double ms, bool after_rerun)
{
    if (ms < 0) ms = 0;
    if (after_rerun) { g.part[Replay] += ms; g.reruns++; } else g.part[Guest] += ms;
}
inline void add_rollback(Gap& g, uint32_t depth) { g.rollbacks++; if (depth > g.depth_max) g.depth_max = depth; }

// A presented frame closed a gap of `gap_ms`: the line for it ("" when not a hitch or past the cap), the gap restarts. `tick` = session-relative tick.
inline std::string close_gap(Match& m, double gap_ms, uint32_t tick, const Clock& c)
{
    std::string s;
    if (gap_ms > kHitchMs) {
        m.hitches++;
        const int k = cause(m.gap, c);
        m.by_cause[k]++;
        if (m.lines < kMaxLines) {
            m.lines++;
            const Gap& g = m.gap; char b[1024];
            const double bnd = g.part[Session] + g.part[Hold] + g.part[Audio] + g.part[Files] + g.part[Server] + g.part[Other];
            int n = std::snprintf(b, sizeof b, "[perf] hitch %u: %.0f ms at tick %u, cause %s | main: boundary %.1f ms (session %.1f, of it snapshot %.1f in %u op(s), gate %.1f; "
                                  "hold %.1f, audio %.1f, sound files %.1f server %.1f, other %.1f; %u boundaries), guest %.1f ms, replay %.1f ms (%u re-run ticks, "
                                  "%u rollbacks, deepest %u)",
                                  m.hitches, gap_ms, tick, cause_name(k), bnd, g.part[Session], g.part[Snapshot], g.snap_ops, g.snap_gate, g.part[Hold], g.part[Audio], g.part[Files],
                                  g.part[Server], g.part[Other], g.boundaries, g.part[Guest], g.part[Replay], g.reruns, g.rollbacks, g.depth_max);
            if (n < 0 || n >= (int)sizeof b) n = (int)sizeof b - 1;
            if (c.ready_late_ms > -1e8) n += std::snprintf(b + n, sizeof b - n, " | clock: flip ready %+.1f ms, taken %+.1f ms",
                                                           c.ready_late_ms, c.take_late_ms > -1e8 ? c.take_late_ms : 0.0);
            else n += std::snprintf(b + n, sizeof b - n, " | clock: no flip ready");
            if (n < (int)sizeof b) n += std::snprintf(b + n, sizeof b - n, ", drain max %.1f ms, present/pump max %.1f ms, %u flips, %u restarts", c.drain_max_ms,
                                                      c.outside_max_ms, c.flips, c.restarts);
            if (c.sampled && n < (int)sizeof b)
                n += std::snprintf(b + n, sizeof b - n, "; at %+.0f ms main was in %s (HLE kind %u id 0x%08X lr 0x%08X)", c.sample_late_ms, stage_name(c.stage), c.kind, c.id, c.lr);
            s = b; s += '\n';
        }
    }
    m.gap = Gap();
    return s;
}

// One summary line for the end of the match: hitches per cause, and how many were not written.
inline std::string summary(const Match& m)
{
    std::string s = "[perf] hitch causes:";
    char b[64];
    for (int k = 0; k < kCauses; k++) if (m.by_cause[k]) { std::snprintf(b, sizeof b, " %s %u", cause_name(k), m.by_cause[k]); s += b; }
    if (!m.hitches) s += " none";
    std::snprintf(b, sizeof b, " | %u lines written, %u not (cap %u)\n", m.lines, m.hitches - m.lines, kMaxLines);
    s += b;
    return s;
}

}  // namespace jph
