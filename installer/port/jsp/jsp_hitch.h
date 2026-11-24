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
//                (main.cpp's present, drain and window pump), and for the first late flip two samples of where the main thread was (boundary
//                stage, or the guest's HLE call and call site).
// Cost: a few QueryPerformanceCounter reads per boundary and per clock pass, two thread censuses for the first flip of a gap that is 8 ms
// late (at 8 and 24 ms), one line per hitch.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace jph {

inline constexpr double kHitchMs = 50.0;       // the same threshold as jpperf::kHitchMs
inline constexpr uint32_t kMaxLines = 60;      // lines per match; later hitches are only counted
inline constexpr double kGuestNominalMs = 22.0;   // guest time of an ordinary tick (33.3 ms minus a ~11 ms boundary): only the excess competes as a cause
inline constexpr double kLateMs = 8.0;         // a flip this late (ready after its deadline) counts as a late guest; also when the clock samples the main thread
// JSP_HITCH_DIAG=0 turns every part of the per-hitch diagnostic off (the frame clock's notes and samples, its mutex, the main thread's stage timing,
// the lines, the snapshot gate split); default on. One read per process (the environment is set before the game starts).
inline bool diag_on_env(const char* v) { return !(v && v[0] == '0'); }
inline constexpr double kRenderMs = 20.0;      // a FIFO drain or present/pump stretch this long counts as the frame clock's own delay

enum Part { Session, Snapshot, Hold, Audio, Files, Server, Other, Guest, Replay, kParts };   // Snapshot is inside Session (the caller files the hold loop's
                                                                                              // updates, and the snapshots they take, as session, not hold)

// The frame clock's view of one gap (jsp_stepper.cpp fills it; jsp_stepper_hitch_take hands it over and starts the next one).
struct Clock {
    double ready_late_ms = -1e9;   // the latest a flip became ready after its deadline (negative: early; -1e9: no flip became ready)
    double take_late_ms = -1e9;    // the latest a flip was taken after its deadline
    double drain_max_ms = 0;       // longest single cellGcm_rsx_process_fifo call in the lockstep clock
    double outside_max_ms = 0;     // longest stretch between two lockstep passes (main.cpp: present, drain, window pump)
    // main.cpp's outer cadence, timed part by part (jsp_stepper_hitch_outer): the longest present (both present calls of a pass), FIFO drain and window pump.
    // In a lockstep match the outer drain does nearly all of the FIFO work (the pass's own drain finds it empty: test7 lines say "drain max 0.0").
    double out_present_ms = 0, out_drain_ms = 0, out_pump_ms = 0;
    uint32_t flips = 0, restarts = 0;
    // Where the main thread was while the first late flip of the gap was not ready: sampled at kLateMs and at 3 x kLateMs past its deadline.
    // stage: g_jsp_main_stage (0 guest code, 1.. boundary stages). tid: the main guest thread's id, 0 = unknown (no id yet, or not alive in the census): then
    // kind / id / lr say nothing. Else kind = its HLE call (0 = guest code), id, call site.
    struct Sample { int stage = 0; uint32_t tid = 0, kind = 0, id = 0, lr = 0; double late_ms = 0; };
    static constexpr int kSamples = 2;
    int samples = 0; Sample s[kSamples];
};

struct Gap {
    double part[kParts] = {};
    double snap_gate = 0;          // of the snapshot time: the gate (waiting for the game's threads to park, then the freeze)
    uint32_t boundaries = 0, snap_ops = 0, reruns = 0, rollbacks = 0, depth_max = 0;
};

enum Cause { CSnapshot, CSession, CHold, CAudio, CSound, COther, CReplay, CGuestLate, CDrain, CPresent, CGuest, CNone, kCauses };
inline const char* cause_name(int c)
{
    static const char* n[kCauses] = { "snapshot", "session", "hold-for-tick", "audio-lock", "sound-barrier", "boundary-other", "rollback-replay",
                                      "guest-late", "fifo-drain", "present-pump", "guest-flip-wait", "unexplained" };
    return c >= 0 && c < kCauses ? n[c] : "?";
}
inline const char* stage_name(int s)   // the same names as jsp_stepper.cpp's main_stage_name (g_jsp_main_stage)
{
    switch (s) { case 0: return "guest-code"; case 1: return "boundary:session"; case 2: return "boundary:hold-for-a-tick"; case 3: return "boundary:audio-lock";
                 case 4: return "boundary:sound-server"; case 5: return "boundary:other"; }
    return "?";
}

// The part that took the most time. Boundary parts count whole; the guest only above kGuestNominalMs (an ordinary tick spends that much there); nothing
// above 0 = unexplained (the line's "unaccounted" may then hold it). A guest winner is split with the frame clock. A drain or a present/pump stretch over
// kRenderMs comes FIRST: the clock thread only sees the flip ready after its drain walked the FIFO to the flip command and after main.cpp gave the loop
// back, so a 40 ms drain or present makes ready_late +35 even when the guest queued the flip on time (review of diag/hitch, S1). Then a flip ready
// kLateMs late = the guest was late (the samples name where); else the guest waited for the flip.
// FIX (test10, honest causes): the guest competes with its excess only, so a normal 12-22 ms save in a gap where the guest took 28-37 ms won as "snapshot"
// (s35: 5 of 6 written lines, 247 "snapshot" hitches against 207 saves over 30 ms). "snapshot" now also needs the snapshot to be the gap's largest part
// against the WHOLE guest time; when the guest took at least as long, the gap goes down the guest branch (its split with the frame clock below).
inline int cause(const Gap& g, const Clock& c)
{
    const double sess = g.part[Session] - g.part[Snapshot];
    struct { int c; double ms; } v[] = {
        { CSnapshot, g.part[Snapshot] }, { CSession, sess > 0 ? sess : 0 }, { CHold, g.part[Hold] }, { CAudio, g.part[Audio] },
        { CSound, g.part[Files] + g.part[Server] }, { COther, g.part[Other] }, { CReplay, g.part[Replay] }, { CGuest, g.part[Guest] - kGuestNominalMs } };
    int best = 0;
    for (int i = 1; i < (int)(sizeof v / sizeof v[0]); i++) if (v[i].ms > v[best].ms) best = i;
    if (v[best].ms <= 0) return CNone;
    if (v[best].c == CSnapshot && g.part[Guest] >= g.part[Snapshot]) best = 7;   // v[7] is the guest: the snapshot is not the largest part
    if (v[best].c != CGuest) return v[best].c;
    if (c.drain_max_ms >= kRenderMs || c.outside_max_ms >= kRenderMs) {   // the outer stretch's own split decides when it is the bigger one
        if (c.drain_max_ms >= c.outside_max_ms) return CDrain;
        return c.out_drain_ms > c.out_present_ms && c.out_drain_ms > c.out_pump_ms ? CDrain : CPresent;
    }
    if (c.ready_late_ms >= kLateMs) return CGuestLate;
    return CGuest;
}

// DIAGNOSTIC (per-match pipeline counts, toolkit 2922): the running totals of the game's pipeline requests that missed the in-memory cache,
// warm (built ahead by the warm-up) and cold (built on request), and the cold build time. main.cpp points this at
// rsx_d3d12_pipeline_counts; it stays null in the host test, and then the summary has no pipelines part.
inline void (*g_pipeline_counts)(long* warm, long* cold, double* cold_ms) = nullptr;
struct Pipes { long warm = 0, cold = 0; double cold_ms = 0; bool ok = false; };
inline Pipes pipes_now()
{
    Pipes p;
    if (g_pipeline_counts) { g_pipeline_counts(&p.warm, &p.cold, &p.cold_ms); p.ok = true; }
    return p;
}

struct Match {
    Gap gap;
    uint32_t hitches = 0, lines = 0;
    uint32_t by_cause[kCauses] = {};
    Pipes pipes0;   // the pipeline totals at the match's first measured frame (close_gap), so the summary counts the fight only
};

// Main thread, per boundary that the gap covers. Every part is clamped at 0 (timer granularity, a caller's subtraction).
inline void add_boundary(Gap& g, double session, double snapshot, double snap_gate, uint32_t snap_ops, double hold, double audio, double files, double server, double total)
{
    auto pos = [](double x) { return x > 0 ? x : 0.0; };
    session = pos(session); snapshot = pos(snapshot); hold = pos(hold); audio = pos(audio); files = pos(files); server = pos(server);
    g.boundaries++;
    g.part[Session] += session; g.part[Snapshot] += snapshot; g.snap_gate += pos(snap_gate); g.snap_ops += snap_ops; g.part[Hold] += hold; g.part[Audio] += audio;
    g.part[Files] += files; g.part[Server] += server;
    g.part[Other] += pos(total - session - hold - audio - files - server);
}
// The slice of the boundary that closes the gap, from its entry to the perf point (native / match poll, the tick state, the watches): boundary "other"
// of the CLOSING gap (review of diag/hitch, S2); the rest of that boundary goes to the next gap through add_boundary.
inline void add_pre(Gap& g, double ms) { if (ms > 0) g.part[Other] += ms; }
// The guest stretch between two boundaries; after an iteration that ran a re-run tick it is replay time.
inline void add_guest(Gap& g, double ms, bool after_rerun)
{
    if (ms < 0) ms = 0;
    if (after_rerun) { g.part[Replay] += ms; g.reruns++; } else g.part[Guest] += ms;
}
inline void add_rollback(Gap& g, uint32_t depth) { g.rollbacks++; if (depth > g.depth_max) g.depth_max = depth; }

// A presented frame closed a gap of `gap_ms`: the line for it ("" when not a hitch or past the cap), the gap restarts. `tick` = session-relative tick.
// "unaccounted" = the gap minus everything filed (boundaries, guest, replay): a hole the parts do not cover (negative: an overlap).
inline std::string close_gap(Match& m, double gap_ms, uint32_t tick, const Clock& c)
{
    std::string s;
    if (!m.pipes0.ok) m.pipes0 = pipes_now();
    if (gap_ms > kHitchMs) {
        m.hitches++;
        const int k = cause(m.gap, c);
        m.by_cause[k]++;
        if (m.lines < kMaxLines) {
            m.lines++;
            const Gap& g = m.gap; char b[1024]; size_t n = 0;
            auto put = [&](const char* fmt, auto... a) {   // bounded append: an oversize line is cut, never overrun
                if (n >= sizeof b - 1) return;
                const int r = std::snprintf(b + n, sizeof b - n, fmt, a...);
                if (r > 0) n += (size_t)r < sizeof b - n ? (size_t)r : sizeof b - n - 1;
            };
            const double bnd = g.part[Session] + g.part[Hold] + g.part[Audio] + g.part[Files] + g.part[Server] + g.part[Other];
            put("[perf] hitch %u: %.0f ms at tick %u, cause %s | main: boundary %.1f ms (session %.1f, of it snapshot %.1f in %u op(s), gate %.1f; "
                "hold %.1f, audio %.1f, sound files %.1f server %.1f, other %.1f; %u boundaries), guest %.1f ms, replay %.1f ms (%u re-run ticks, "
                "%u rollbacks, deepest %u), unaccounted %.1f ms",
                m.hitches, gap_ms, tick, cause_name(k), bnd, g.part[Session], g.part[Snapshot], g.snap_ops, g.snap_gate, g.part[Hold], g.part[Audio], g.part[Files],
                g.part[Server], g.part[Other], g.boundaries, g.part[Guest], g.part[Replay], g.reruns, g.rollbacks, g.depth_max,
                gap_ms - bnd - g.part[Guest] - g.part[Replay]);
            if (c.ready_late_ms > -1e8) put(" | clock: flip ready %+.1f ms", c.ready_late_ms); else put("%s", " | clock: no flip ready");
            if (c.take_late_ms > -1e8) put(", taken %+.1f ms", c.take_late_ms); else put("%s", ", not taken");
            put(", drain max %.1f ms, present/pump max %.1f ms (present %.1f, drain %.1f, pump %.1f), %u flips, %u restarts", c.drain_max_ms, c.outside_max_ms,
                c.out_present_ms, c.out_drain_ms, c.out_pump_ms, c.flips, c.restarts);
            for (int i = 0; i < c.samples && i < Clock::kSamples; i++) {
                const Clock::Sample& x = c.s[i];
                put("%s at %+.0f ms main was in %s", i ? "," : ";", x.late_ms, stage_name(x.stage));
                if (x.tid) put(" (tid %u HLE kind %u id 0x%08X lr 0x%08X)", x.tid, x.kind, x.id, x.lr); else put("%s", " (tid ?)");
            }
            s.assign(b, n); s += '\n';
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
    std::snprintf(b, sizeof b, " | %u lines written, %u not (cap %u)", m.lines, m.hitches - m.lines, kMaxLines);
    s += b;
    if (m.pipes0.ok) {   // DIAGNOSTIC (toolkit 2922): pipelines the fight asked for, warm (built ahead) vs cold (built during the fight)
        const Pipes p = pipes_now();
        char pb[128];
        std::snprintf(pb, sizeof pb, " | pipelines: %ld warm, %ld cold (%.0f thread-ms building)", p.warm - m.pipes0.warm, p.cold - m.pipes0.cold,
                      p.cold_ms - m.pipes0.cold_ms);
        s += pb;
    }
    s += '\n';
    return s;
}

}  // namespace jph
