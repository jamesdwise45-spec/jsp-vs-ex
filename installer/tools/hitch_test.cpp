// hitch_test.cpp -- host test of the per-hitch line (installer/port/jsp/jsp_hitch.h): the gap's parts, the cause rule, the line, the per-match cap and
// the summary. No game, no Windows. Run by hitch_test.sh.
#include "jsp_hitch.h"

#include <cstdio>
#include <string>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// An ordinary tick: an 11 ms boundary (session 5 with a 4 ms save, sound 4, audio 0.1) and 22 ms of guest time.
static void ordinary(jph::Gap& g)
{
    jph::add_boundary(g, 5.0, 4.0, 3.0, 1, 0.0, 0.1, 3.0, 1.0, 11.0);
    jph::add_guest(g, 22.0, false);
}

int main()
{
    // no hitch: no line, the gap restarts, nothing counted
    {
        jph::Match m; ordinary(m.gap);
        CHECK(jph::close_gap(m, 33.3, 100, jph::Clock()).empty());
        CHECK(m.hitches == 0 && m.lines == 0);
        CHECK(m.gap.boundaries == 0 && m.gap.part[jph::Guest] == 0);
    }
    // a slow save (the joiner's 43-79 ms saves in the test6 logs): cause snapshot, the line names it with its gate time
    {
        jph::Match m;
        jph::add_boundary(m.gap, 48.0, 45.3, 41.0, 1, 0.0, 0.1, 3.0, 1.0, 53.0);
        jph::add_guest(m.gap, 23.0, false);
        jph::Clock c; c.ready_late_ms = 30.0; c.take_late_ms = 31.0; c.flips = 1; c.restarts = 1;
        const std::string s = jph::close_gap(m, 76.0, 1855, c);
        CHECK(has(s, "[perf] hitch 1: 76 ms at tick 1855, cause snapshot |"));
        CHECK(has(s, "snapshot 45.3 in 1 op(s), gate 41.0"));
        CHECK(has(s, "flip ready +30.0 ms, taken +31.0 ms"));
        CHECK(has(s, "1 flips, 1 restarts"));
        CHECK(s.back() == '\n' && s.find('\n') == s.size() - 1);   // one line
        CHECK(m.by_cause[jph::CSnapshot] == 1);
    }
    // the sound barrier (a file read or the sound server cycle at the boundary)
    {
        jph::Match m;
        jph::add_boundary(m.gap, 4.0, 0.0, 0.0, 0, 0.0, 0.1, 38.0, 9.0, 52.0);
        jph::add_guest(m.gap, 22.0, false);
        CHECK(has(jph::close_gap(m, 74.0, 10, jph::Clock()), "cause sound-barrier"));
    }
    // guest time over the nominal with the flip ready late: guest-late, with the sample (main thread in guest code, an HLE call)
    {
        jph::Match m; ordinary(m.gap); m.gap.part[jph::Guest] += 45.0;
        jph::Clock c; c.ready_late_ms = 40.0; c.take_late_ms = 41.0;
        c.samples = 2; c.s[0].stage = 0; c.s[0].tid = 1; c.s[0].kind = 3; c.s[0].id = 0x1F953B9F; c.s[0].lr = 0x0046A1F0; c.s[0].late_ms = 8.4;
        c.s[1].stage = 4; c.s[1].late_ms = 24.2;
        const std::string s = jph::close_gap(m, 78.0, 3898, c);
        CHECK(has(s, "cause guest-late"));
        CHECK(has(s, "; at +8 ms main was in guest-code (tid 1 HLE kind 3 id 0x1F953B9F lr 0x0046A1F0), at +24 ms main was in boundary:sound-server (tid ?)"));
        CHECK(!has(jph::close_gap(m, 20.0, 1, c), "main was in"));   // no hitch, no line
    }
    // guest time over the nominal, flip ready on time, a long FIFO drain: the frame clock's own work
    {
        jph::Match m; ordinary(m.gap); m.gap.part[jph::Guest] += 40.0;
        jph::Clock c; c.ready_late_ms = -2.0; c.take_late_ms = 38.0; c.drain_max_ms = 41.0; c.outside_max_ms = 3.0;
        CHECK(has(jph::close_gap(m, 75.0, 5, c), "cause fifo-drain"));
        jph::add_boundary(m.gap, 5.0, 0.0, 0.0, 0, 0.0, 0.1, 3.0, 1.0, 11.0); jph::add_guest(m.gap, 62.0, false);
        jph::Clock p; p.ready_late_ms = 0.5; p.drain_max_ms = 2.0; p.outside_max_ms = 37.0;
        CHECK(has(jph::close_gap(m, 75.0, 6, p), "cause present-pump"));
        jph::add_boundary(m.gap, 5.0, 0.0, 0.0, 0, 0.0, 0.1, 3.0, 1.0, 11.0); jph::add_guest(m.gap, 62.0, false);
        jph::Clock q; q.ready_late_ms = 1.0; q.drain_max_ms = 3.0; q.outside_max_ms = 4.0;
        CHECK(has(jph::close_gap(m, 75.0, 7, q), "cause guest-flip-wait"));
    }
    // S1 (review): the clock sees the flip ready only after its drain / after main.cpp's present: ready +35 with a 40 ms drain is the drain, not a late
    // guest; the same with a 40 ms present/pump stretch; a late flip with short drains stays guest-late
    {
        jph::Match m; ordinary(m.gap); m.gap.part[jph::Guest] += 45.0;
        jph::Clock c; c.ready_late_ms = 35.0; c.take_late_ms = 36.0; c.drain_max_ms = 40.0; c.outside_max_ms = 2.0;
        CHECK(has(jph::close_gap(m, 78.0, 1, c), "cause fifo-drain"));
        ordinary(m.gap); m.gap.part[jph::Guest] += 45.0;
        jph::Clock p; p.ready_late_ms = 35.0; p.take_late_ms = 36.0; p.drain_max_ms = 3.0; p.outside_max_ms = 40.0;
        CHECK(has(jph::close_gap(m, 78.0, 2, p), "cause present-pump"));
        ordinary(m.gap); m.gap.part[jph::Guest] += 45.0;
        jph::Clock q; q.ready_late_ms = 35.0; q.take_late_ms = 36.0; q.drain_max_ms = 3.0; q.outside_max_ms = 2.0;
        CHECK(has(jph::close_gap(m, 78.0, 3, q), "cause guest-late"));
    }
    // S2 (review): the slice before the perf point is boundary "other" of the closing gap; the line says what nothing covers
    {
        jph::Match m; ordinary(m.gap); jph::add_pre(m.gap, 40.0); jph::add_pre(m.gap, -3.0);
        const std::string s = jph::close_gap(m, 80.0, 9, jph::Clock());
        CHECK(has(s, "cause boundary-other")); CHECK(has(s, "other 41.9"));
        CHECK(has(s, "unaccounted 7.0 ms"));   // 80 - 51 boundary - 22 guest
        CHECK(has(s, "clock: no flip ready, not taken"));
    }
    // S3 (review) and the clamps: a negative part is filed as 0, "session minus snapshot" never goes below 0 and never wins
    {
        jph::Match m;
        jph::add_boundary(m.gap, 2.0, 25.0, 20.0, 1, -4.0, -0.1, 0.0, 0.0, 10.0);
        CHECK(m.gap.part[jph::Hold] == 0.0 && m.gap.part[jph::Audio] == 0.0 && m.gap.part[jph::Other] == 8.0);
        CHECK(jph::cause(m.gap, jph::Clock()) == jph::CSnapshot);
        jph::Gap h; jph::add_boundary(h, 30.0, 25.0, 20.0, 1, 0.0, 0.0, 0.0, 0.0, 30.0);   // a hold that ended in an update with a 25 ms save: session 30, hold 0
        CHECK(jph::cause(h, jph::Clock()) == jph::CSnapshot);
    }
    // N1: nothing above 0 (all time unaccounted) is unexplained, not snapshot
    {
        jph::Match m; jph::add_guest(m.gap, 10.0, false);
        CHECK(has(jph::close_gap(m, 60.0, 4, jph::Clock()), "cause unexplained"));
        CHECK(has(jph::summary(m), "unexplained 1"));
    }
    // truncation: an absurd line is cut at the buffer, one line, no overrun
    {
        jph::Match m; ordinary(m.gap); m.gap.part[jph::Guest] = 1e300; m.gap.part[jph::Replay] = 1e300;
        jph::Clock c; c.ready_late_ms = 1e7; c.take_late_ms = 1e7; c.drain_max_ms = 1e300; c.samples = 2;
        const std::string s = jph::close_gap(m, 1e300, 1, c);
        CHECK(s.size() <= 1024); CHECK(s.size() > 900); CHECK(s.back() == '\n' && s.find('\n') == s.size() - 1);
    }
    // test7: the outer cadence's own split decides a long present/pump stretch: its FIFO drain is fifo-drain, a present or a pump stays present-pump;
    // the line names the three parts; JSP_HITCH_DIAG parsing (unset / other = on, "0" = off)
    {
        jph::Match m; ordinary(m.gap); m.gap.part[jph::Guest] += 45.0;
        jph::Clock c; c.ready_late_ms = 40.0; c.take_late_ms = 41.0; c.outside_max_ms = 55.0; c.out_present_ms = 3.0; c.out_drain_ms = 50.0; c.out_pump_ms = 1.0;
        const std::string s = jph::close_gap(m, 80.0, 1, c);
        CHECK(has(s, "cause fifo-drain")); CHECK(has(s, "present/pump max 55.0 ms (present 3.0, drain 50.0, pump 1.0)"));
        ordinary(m.gap); m.gap.part[jph::Guest] += 45.0;
        jph::Clock p = c; p.out_present_ms = 50.0; p.out_drain_ms = 3.0;
        CHECK(has(jph::close_gap(m, 80.0, 2, p), "cause present-pump"));
        ordinary(m.gap); m.gap.part[jph::Guest] += 45.0;
        jph::Clock u = c; u.out_pump_ms = 50.0; u.out_drain_ms = 3.0;
        CHECK(has(jph::close_gap(m, 80.0, 3, u), "cause present-pump"));
        CHECK(jph::diag_on_env(nullptr)); CHECK(jph::diag_on_env("1")); CHECK(jph::diag_on_env("")); CHECK(!jph::diag_on_env("0"));
    }
    // a rollback: the re-run ticks' time is replay; the rollback is counted; no flip ready in the gap is said
    {
        jph::Match m;
        jph::add_boundary(m.gap, 12.0, 10.0, 4.0, 2, 0.0, 0.0, 3.0, 1.0, 17.0);   // restore + doomed save
        jph::add_rollback(m.gap, 2);
        jph::add_guest(m.gap, 14.0, true);
        jph::add_boundary(m.gap, 7.0, 6.0, 3.0, 1, 0.0, 0.0, 3.0, 1.0, 12.0);
        jph::add_guest(m.gap, 14.0, true);
        jph::add_boundary(m.gap, 5.0, 4.0, 3.0, 1, 0.0, 0.1, 3.0, 1.0, 11.0);
        jph::add_guest(m.gap, 20.0, false);
        const std::string s = jph::close_gap(m, 98.0, 3083, jph::Clock());
        CHECK(has(s, "replay 28.0 ms (2 re-run ticks, 1 rollbacks, deepest 2)"));
        CHECK(has(s, "3 boundaries"));
        CHECK(has(s, "clock: no flip ready"));
        CHECK(has(s, "cause rollback-replay"));   // 28 ms of re-run ticks beat 20 ms of snapshots (restore, doomed save, re-run saves)
    }
    // the cap: kMaxLines lines, later hitches only counted; the summary says so
    {
        jph::Match m; int lines = 0;
        for (uint32_t i = 0; i < jph::kMaxLines + 5; i++) { ordinary(m.gap); m.gap.part[jph::Audio] += 40.0; if (!jph::close_gap(m, 70.0, i, jph::Clock()).empty()) lines++; }
        CHECK(lines == (int)jph::kMaxLines);
        CHECK(m.hitches == jph::kMaxLines + 5 && m.by_cause[jph::CAudio] == jph::kMaxLines + 5);
        const std::string s = jph::summary(m);
        CHECK(has(s, "[perf] hitch causes: audio-lock 65 | 60 lines written, 5 not (cap 60)"));
        CHECK(has(jph::summary(jph::Match()), "hitch causes: none | 0 lines written, 0 not"));
    }
    // the pipelines part (toolkit 2922): counted from the match's first measured frame; absent while no counter is wired (the test default)
    {
        static long warm = 5, cold = 2; static double ms = 30.0;
        CHECK(!has(jph::summary(jph::Match()), "pipelines"));
        jph::g_pipeline_counts = [](long* w, long* c, double* t) { *w = warm; *c = cold; *t = ms; };
        jph::Match m; ordinary(m.gap); jph::close_gap(m, 33.0, 1, jph::Clock());
        warm += 7; cold += 3; ms += 120.0;
        const std::string s = jph::summary(m);
        CHECK(has(s, " | pipelines: 7 warm, 3 cold (120 ms building)\n"));
        CHECK(has(s, "hitch causes: none | 0 lines written, 0 not (cap 60) | pipelines"));
        jph::g_pipeline_counts = nullptr;
    }
    // a boundary whose stages exceed its total (timer granularity) adds no negative "other"
    {
        jph::Gap g; jph::add_boundary(g, 5.0, 0.0, 0.0, 0, 0.0, 0.0, 3.0, 3.0, 10.0);
        CHECK(g.part[jph::Other] == 0.0);
        jph::add_guest(g, -1.0, false); CHECK(g.part[jph::Guest] == 0.0);
    }
    std::printf("hitch_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
