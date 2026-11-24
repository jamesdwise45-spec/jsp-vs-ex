// net_delay_rule_test.cpp -- host test of the adaptive input delay decision (installer/port/jsp/jsp_net_delay_rule.h): rule 1 is the
// 2026-11-21 formula unchanged, rule 2's percentile trip, high-ping margin, fight-start guard, depth trigger, hold and one-tick lowering, and the RTT
// sampling fix (only a round trip whose GekkoNet count is new enters the window). test10: the start at 3, the deadband against 2 <-> 3 flapping, no
// depth trigger while the time sync stretches for a slow peer, and the rematch carry (carry_keep / carry_start).
// A fake clock and fake stats; no GekkoNet. Run by net_delay_rule_test.sh.
#include "jsp_net_delay_rule.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// The fake session models GekkoNet: a new single round trip every ping_every ms (its count rtt_seq + 1), and between two of them every stats read returns
// the SAME last round trip and count. fresh_every_poll = the old fake (and the bug the review found): every read is a new measurement. pings = false: no
// answer arrives any more (the count stays), as in a stall.
struct Sim {
    jpdr::Params p; jpdr::State s; int cur = 2; uint64_t t = 100000;
    std::function<double(uint64_t)> rtt = [](uint64_t) { return 100.0; };   // single round trip measured at time t (ms since start)
    double avg = 100, jit = 2;
    uint32_t stretch = 0;   // Reading::stretch_us
    uint64_t t0 = 0, step_ms = 33, ping_every = 500;
    bool fresh_every_poll = false, pings = true;
    uint32_t seq = 0; double last = -1; uint32_t reads = 0;
    int changes = 0; const char* last_why = "";
    struct Seen { uint64_t t; int cur, want; double ping, spread, need; };
    std::vector<Seen> seen;   // every trip reading (the decision's inputs), for the equivalence check
    jpdr::Reading read()
    {
        reads++;
        if (fresh_every_poll) { seq++; last = rtt(t - t0); }
        else if (pings) { const uint32_t k = (uint32_t)((t - t0) / ping_every) + 1; if (k != seq) { seq = k; last = rtt(t - t0); } }
        jpdr::Reading r; r.ping_avg = avg; r.jitter = jit; r.last_ping = last; r.rtt_seq = seq; r.cur = cur; r.stretch_us = stretch; return r;
    }
    // runs ms of live ticks (one per step_ms); returns the delay
    int run(uint64_t ms)
    {
        const uint64_t end = t + ms;
        for (; t < end; t += step_ms) {
            if (!t0) t0 = t;
            const jpdr::Decision d = jpdr::step(s, p, t, [&] { return read(); });
            if (d.why[0] == 't') seen.push_back({t - t0, d.cur, d.want, d.ping, d.jitter, d.need_ms});
            if (d.want >= 0 && d.want != cur) { cur = d.want; changes++; last_why = d.why; }
        }
        return cur;
    }
};

int main()
{
    // rule 1: no reading before 10 s; ping 129, jitter 36 -> ceil(((129 - 33.3) / 2 + 36 + 8) / 33.3) = 3, after two readings
    {
        Sim m; m.p.rule = 1; m.p.clamp(); m.avg = 129; m.jit = 36; m.cur = 2;
        CHECK(m.run(9900) == 2);
        CHECK(m.run(2000) == 2);   // first reading at 10 s: one vote
        CHECK(m.run(2000) == 3);   // second reading
        CHECK(m.changes == 1);
        // a two-tick jump goes at once: ping 250, jitter 40 -> ceil((108.3 + 40 + 8) / 33.3) = 5 -> 4
        Sim j; j.p.rule = 1; j.p.clamp(); j.avg = 250; j.jit = 40;
        CHECK(j.run(10100) == 4);
        // lowering needs five readings
        Sim l; l.p.rule = 1; l.p.clamp(); l.avg = 20; l.jit = 1; l.cur = 3;
        CHECK(l.run(10100 + 3 * 2000) == 3);   // readings at 10, 12, 14, 16 s: four votes
        CHECK(l.run(2000) == 1);   // the earlier rule lowers to the target at once
    }
    // rule 2 defaults: 99 ms steady (+ 16 ms answer wait in the samples): one way (115 - 33.3) / 2 = 40.8, spread 0, margin 16 -> 2 ticks
    {
        Sim m; m.p.clamp(); m.rtt = [](uint64_t) { return 115.0; };
        CHECK(m.run(30000) == 2);
        CHECK(m.changes == 0);
    }
    // rule 2: +-36 jitter on 99 ms (RTT 115 +- 50): p90 well above the median -> 3 or 4 ticks, reached within the first 12 s
    {
        Sim m; m.p.clamp(); unsigned k = 1;
        m.rtt = [&k](uint64_t) { k = k * 1103515245u + 12345u; return 115.0 + ((double)((k >> 8) % 1001) / 1000.0 * 100.0 - 50.0); };
        m.run(12000);
        CHECK(m.cur >= 3);
        m.run(30000);
        CHECK(m.cur >= 3 && m.cur <= 4);
    }
    // the high-ping margin: median 75 (below 80): (75 - 33.3) / 2 + 8 = 28.9 -> 1; median 85: (85 - 33.3) / 2 + 16 = 41.9 -> 2
    {
        Sim a; a.p.clamp(); a.cur = 1; a.rtt = [](uint64_t) { return 75.0; };
        CHECK(a.run(30000) == 1);
        Sim b; b.p.clamp(); b.cur = 1; b.rtt = [](uint64_t) { return 85.0; };
        CHECK(b.run(30000) == 2);
    }
    // the fight-start guard: round trips of 400 ms in the first 4 s, 60 ms after: at most start + 1 before 10 s, back down later
    {
        Sim m; m.p.clamp(); m.rtt = [](uint64_t t) { return t < 4500 ? 400.0 : 60.0; };
        int hi = 0; for (int i = 0; i < 10; i++) { m.run(1000); hi = std::max(hi, m.cur); }
        CHECK(hi <= 3);
        m.run(40000);
        CHECK(m.cur == 1);   // (60 - 30) / 2 + 8 = 23 ms -> 1 tick, lowered one tick at a time
    }
    // a slow link seen early: 129 ms steady + wait -> 3 ticks at the first reading (4 s), not at 10 s
    {
        Sim m; m.p.clamp(); m.rtt = [](uint64_t) { return 145.0; };   // (145 - 33.3) / 2 + 16 = 71.8 -> 3
        m.run(4100);
        CHECK(m.cur == 3);
    }
    // the depth trigger: two deep rollbacks (as two events) within 10 s raise by one at once and hold the delay 20 s
    {
        Sim m; m.p.clamp(); m.rtt = [](uint64_t) { return 60.0; };   // the trip alone wants 1
        m.cur = 2;
        m.run(5000);
        CHECK(m.cur == 2);   // the guard: median of 60 -> 1, but down needs five readings
        jpdr::note_rollback(m.s, m.p, m.t, 6); jpdr::note_rollback(m.s, m.p, m.t + 33, 6);   // one event (consecutive ticks)
        m.run(1000);
        CHECK(m.cur == 2);
        m.t += 3000; jpdr::note_rollback(m.s, m.p, m.t, 5);
        m.run(100);
        CHECK(m.cur == 3); CHECK(std::string(m.last_why) == "rollbacks");
        m.run(19000);
        CHECK(m.cur == 3);   // held
        m.run(30000);
        CHECK(m.cur < 3);    // then down, one tick at a time
        // shallow rollbacks never trigger; DEEP_COUNT=0 turns it off
        Sim n; n.p.clamp(); n.rtt = [](uint64_t) { return 60.0; }; n.run(5000);
        for (int i = 0; i < 6; i++) { n.t += 1000; jpdr::note_rollback(n.s, n.p, n.t, 2); n.run(33); }
        CHECK(n.s.deep_n == 0 && n.s.deep_raises == 0);
        Sim o; o.p.deep_count = 0; o.p.clamp(); o.rtt = [](uint64_t) { return 60.0; }; o.run(5000);
        for (int i = 0; i < 4; i++) { o.t += 1000; jpdr::note_rollback(o.s, o.p, o.t, 8); o.run(33); }
        CHECK(o.s.deep_raises == 0);
        // rollbacks before the first reading time are the fight-start stall: not counted
        Sim q; q.p.clamp(); q.rtt = [](uint64_t) { return 60.0; }; q.run(100);
        jpdr::note_rollback(q.s, q.p, q.t, 8); q.t += 1000; jpdr::note_rollback(q.s, q.p, q.t, 8);
        CHECK(q.s.deep_n == 0);
    }
    // settled(): the delay held longest, only after 20 s; ties go to the higher
    {
        jpdr::State st; uint64_t t = 1000;
        for (int i = 0; i < 300; i++, t += 33) jpdr::note_time(st, t, 2);   // ~10 s at 2
        CHECK(jpdr::settled(st) == -1);
        for (int i = 0; i < 400; i++, t += 33) jpdr::note_time(st, t, 3);   // ~13 s at 3
        CHECK(jpdr::settled(st) == 3);
    }
    // the guard's mirrored spread: a +-36 ms link read at 4 s already wants 3 (median 115, 10th percentile ~75: (115 - 33.3) / 2 + 40 + 16 > 66.7)
    {
        Sim m; m.p.clamp(); unsigned k = 7;
        m.rtt = [&k](uint64_t) { k = k * 1103515245u + 12345u; return 115.0 + ((double)((k >> 8) % 1001) / 1000.0 * 100.0 - 50.0); };
        m.run(4100);
        CHECK(m.cur == 3);
        Sim n; n.p.guard_spread = 0; n.p.clamp(); unsigned k2 = 7;
        n.rtt = [&k2](uint64_t) { k2 = k2 * 1103515245u + 12345u; return 115.0 + ((double)((k2 >> 8) % 1001) / 1000.0 * 100.0 - 50.0); };
        n.run(4100);
        CHECK(n.cur == 2);   // the median alone: (115 - 33.3) / 2 + 16 = 56.9 -> 2
    }
    // FIX (RTT sampling): polls with no new ping answer add nothing. GekkoNet answers stop at 9 s (a stall): the window keeps the samples it had, however
    // often the stats are read, and later readings decide on exactly those samples.
    {
        Sim m; m.p.clamp(); m.rtt = [](uint64_t t) { return 100.0 + (double)(t / 500 % 7) * 10.0; };
        m.run(9000);
        const int n_before = m.s.n; const uint32_t taken = m.s.rtt_taken, seq = m.seq;
        CHECK(n_before >= 13 && n_before <= 15);   // answers at 2.0 .. 9.0 s, one per 500 ms
        m.pings = false;
        const uint32_t reads0 = m.reads;
        m.run(20000);
        CHECK(m.reads - reads0 >= 70);           // polled every 250 ms all along
        CHECK(m.s.n == n_before); CHECK(m.s.rtt_taken == taken); CHECK(m.seq == seq);
        CHECK(m.s.rtt_stale >= 70);
        // the readings after the stall all saw the same window: the same median and spread every time
        bool same = true; double p0 = -1, s0 = -1;
        for (const Sim::Seen& e : m.seen) if (e.t >= 12000) { if (p0 < 0) { p0 = e.ping; s0 = e.spread; } else if (e.ping != p0 || e.spread != s0) same = false; }
        CHECK(p0 > 0 && same);
        // the old fake (every read a new measurement) filled the window with copies of the last answer: the bug. With the count, the same reads add nothing.
        Sim b; b.p.clamp(); b.pings = false; b.rtt = [](uint64_t) { return 100.0; };
        b.seq = 1; b.last = 300;   // one answer before the match: count 1
        b.run(30000);
        CHECK(b.s.n == 1); CHECK(b.s.rtt_taken == 1);
        CHECK(b.seen.empty());   // fewer than 3 samples: no reading decides
        CHECK(b.cur == 2);
    }
    // FIX (RTT sampling): with a genuinely new round trip at every poll, the window, the median, the p90 spread and every delay decision are the same as
    // before the fix. Old: a poll every 500 ms, each read a new measurement. New: a poll every 250 ms, GekkoNet answering every 500 ms (every other poll is
    // stale and dropped). 25 ms ticks so that both polls land on the answer times exactly.
    {
        auto link = [](uint64_t t) { const uint32_t k = (uint32_t)(t / 500) * 2654435761u; return 115.0 + (double)((k >> 8) % 101) - 50.0 + (t < 4000 ? 200.0 : 0.0); };
        Sim a; a.p.sample_ms = 500; a.p.clamp(); a.fresh_every_poll = true; a.step_ms = 25; a.rtt = link; a.cur = 1;
        Sim b; b.p.clamp(); b.step_ms = 25; b.rtt = link; b.cur = 1;
        CHECK(b.p.sample_ms == 250);
        a.run(60000); b.run(60000);
        CHECK(a.seen.size() == b.seen.size() && a.seen.size() >= 25);
        bool same = a.seen.size() == b.seen.size();
        for (size_t i = 0; same && i < a.seen.size(); i++) {
            const Sim::Seen& x = a.seen[i]; const Sim::Seen& y = b.seen[i];
            same = x.t == y.t && x.cur == y.cur && x.want == y.want && x.ping == y.ping && x.spread == y.spread && x.need == y.need;
            if (!same) std::printf("  differs at reading %zu: t %llu/%llu cur %d/%d want %d/%d median %.1f/%.1f spread %.1f/%.1f\n", i, (unsigned long long)x.t,
                                   (unsigned long long)y.t, x.cur, y.cur, x.want, y.want, x.ping, y.ping, x.spread, y.spread);
        }
        CHECK(same);
        CHECK(a.changes == b.changes && a.cur == b.cur);
        CHECK(a.changes >= 1);   // the link is lively enough that the rule did move
        CHECK(b.s.rtt_stale >= b.s.rtt_taken - 2);   // every other poll found nothing new
        // and the window sorted is the same multiset
        float va[jpdr::kMaxSamples], vb[jpdr::kMaxSamples];
        for (int i = 0; i < a.s.n; i++) va[i] = a.s.rtt[i];
        for (int i = 0; i < b.s.n; i++) vb[i] = b.s.rtt[i];
        CHECK(a.s.n == b.s.n && a.s.n == 16);
        std::sort(va, va + a.s.n); std::sort(vb, vb + b.s.n);
        CHECK(std::equal(va, va + a.s.n, vb));
        CHECK(jpdr::percentile(va, a.s.n, 90) == jpdr::percentile(vb, b.s.n, 90));
    }
    // the range: MAX caps every decision
    {
        Sim m; m.p.hi = 3; m.p.clamp(); m.rtt = [](uint64_t) { return 400.0; };
        CHECK(m.run(30000) == 3);
    }
    // time sync stretch: unchanged up to 1.5 frames ahead; steeper beyond; capped; the earlier rule with gain 1000 / max 2000
    {
        CHECK(jpdr::timesync_stretch_us(0.0f, 5000, 25000) == 0);
        CHECK(jpdr::timesync_stretch_us(0.5f, 5000, 25000) == 0);
        CHECK(jpdr::timesync_stretch_us(1.0f, 5000, 25000) == 500);
        CHECK(jpdr::timesync_stretch_us(1.5f, 5000, 25000) == 1000);
        CHECK(jpdr::timesync_stretch_us(-3.0f, 5000, 25000) == 0);
        CHECK(std::abs((int)jpdr::timesync_stretch_us(4.7f, 5000, 25000) - 17000) <= 1);   // ~4.7 frames ahead holds a 20 ticks/s peer (33.3 + 17 ms a tick)
        CHECK(jpdr::timesync_stretch_us(8.0f, 5000, 25000) == 25000);
        for (float a = 0.0f; a < 8.0f; a += 0.25f)   // the earlier rule: min(2000, (a - 0.5) * 1000)
            CHECK(jpdr::timesync_stretch_us(a, 1000, 2000) == (a > 0.5f ? (uint32_t)std::min(2000.0f, (a - 0.5f) * 1000.0f) : 0u));
    }
    // test10, the start at 3 (the manifest default): a link that needs less still comes down by the normal votes, one tick at a time
    {
        Sim a; a.p.clamp(); a.cur = 3; a.rtt = [](uint64_t) { return 60.0; };   // (60 - 30) / 2 + 8 = 23 ms -> 1
        CHECK(a.run(11000) == 3);   // readings at 4..10 s: five calm votes are not in yet
        CHECK(a.run(2000) == 2);    // the fifth at 12 s: one tick down
        CHECK(a.run(20000) == 1);
        Sim b; b.p.clamp(); b.cur = 3; b.rtt = [](uint64_t) { return 115.0; };  // 56.8 ms: 2 ticks, and 56.8 + 4 still fits in 2
        CHECK(b.run(40000) == 2); CHECK(b.changes == 1);
        // the guard with a start of 3: before 10 s no decision goes above 4
        Sim c; c.p.clamp(); c.cur = 3; c.rtt = [](uint64_t) { return 400.0; };
        CHECK(c.run(9900) == 4); CHECK(c.s.start_delay == 3);
    }
    // test10, the deadband (4 ms): a need just over a tick boundary keeps the delay; past the band it moves
    {
        // median 138 (margin 16): need (138 - 33.3) / 2 + 16 = 68.3 ms, 1.7 ms over 2 ticks: kept at 2 (without the band: 3)
        Sim a; a.p.clamp(); a.cur = 2; a.rtt = [](uint64_t) { return 138.0; };
        CHECK(a.run(40000) == 2); CHECK(a.changes == 0);
        Sim a0; a0.p.deadband_ms = 0; a0.p.clamp(); a0.cur = 2; a0.rtt = [](uint64_t) { return 138.0; };
        CHECK(a0.run(40000) == 3);
        // median 146: need 72.3 ms, past 66.7 + 4: up at the first reading
        Sim b; b.p.clamp(); b.cur = 2; b.rtt = [](uint64_t) { return 146.0; };
        b.run(4100); CHECK(b.cur == 3);
        // down from 3 needs the need + 4 inside 2 ticks: median 130 -> 64.3 ms, + 4 = 68.3 > 66.7: kept; median 120 -> 59.3 ms: down
        Sim c; c.p.clamp(); c.cur = 3; c.rtt = [](uint64_t) { return 130.0; };
        CHECK(c.run(60000) == 3); CHECK(c.changes == 0);
        Sim d; d.p.clamp(); d.cur = 3; d.rtt = [](uint64_t) { return 120.0; };
        CHECK(d.run(30000) == 2);
        // a delay below MIN still goes up to MIN at once
        Sim e; e.p.lo = 2; e.p.clamp(); e.cur = 1; e.rtt = [](uint64_t) { return 40.0; };
        e.run(4100); CHECK(e.cur == 2);
        // the t8 a / t6-full case: a ~132-137 ms link whose need moves across 66.7 ms (66.3 / 67.6 / 67.8) every 16 s. With the band it stays at 3;
        // without it, it flaps 3 -> 2 -> 3.
        auto link = [](uint64_t t) { const uint64_t k = t / 16000; return k % 2 ? 134.0 : (k % 4 == 0 ? 136.6 : 137.0); };
        Sim f; f.p.clamp(); f.cur = 3; f.rtt = link;
        CHECK(f.run(180000) == 3); CHECK(f.changes == 0);
        Sim f0; f0.p.deadband_ms = 0; f0.p.clamp(); f0.cur = 3; f0.rtt = link;
        f0.run(180000); CHECK(f0.changes >= 4);
    }
    // test10, no depth trigger while this PC stretches its ticks for a slow peer (over 3000 us): no raise, no hold; at 3000 us or less it fires as before
    {
        Sim m; m.p.clamp(); m.cur = 2; m.rtt = [](uint64_t) { return 60.0; };
        m.run(5000);
        m.stretch = 8000;
        jpdr::note_rollback(m.s, m.p, m.t, 6); m.t += 1000; jpdr::note_rollback(m.s, m.p, m.t, 6);
        m.run(100);
        CHECK(m.cur == 2); CHECK(m.s.deep_skips == 1); CHECK(m.s.stretch_us == 8000); CHECK(m.s.hold_until == 0); CHECK(m.s.deep_raises == 0);
        m.run(30000);
        CHECK(m.cur == 1);   // and nothing held the trip rule's lowering
        m.stretch = 3000;
        jpdr::note_rollback(m.s, m.p, m.t, 6); m.t += 1000; jpdr::note_rollback(m.s, m.p, m.t, 6);
        m.run(100);
        CHECK(m.cur == 2); CHECK(std::string(m.last_why) == "rollbacks"); CHECK(m.s.deep_skips == 1);
        // DEEP_STRETCH_US=0: never skipped
        Sim n; n.p.deep_stretch_us = 0; n.p.clamp(); n.cur = 2; n.rtt = [](uint64_t) { return 60.0; };
        n.run(5000); n.stretch = 25000;
        jpdr::note_rollback(n.s, n.p, n.t, 6); n.t += 1000; jpdr::note_rollback(n.s, n.p, n.t, 6);
        n.run(100);
        CHECK(n.cur == 3); CHECK(n.s.deep_skips == 0);
    }
    // test10, the rematch carry: the next match on the same link starts where the last one settled; another link, a short match or no key: the default
    {
        jpdr::Params p; p.clamp();
        jpdr::Carry c;
        CHECK(jpdr::carry_start(c, 0x1234, 3, p) == 3);   // nothing kept yet
        Sim m1; m1.p.clamp(); m1.cur = 3;
        m1.rtt = [](uint64_t t) { return t < 6000 ? 200.0 : 240.0; };   // 200: (200 - 33.3) / 2 + 16 = 99.3 ms -> 3; 240: 119.3 ms -> 4
        for (int i = 0; i < 1200; i++) { m1.run(33); jpdr::note_time(m1.s, m1.t, m1.cur); }
        CHECK(m1.cur == 4); CHECK(jpdr::settled(m1.s) == 4);
        jpdr::carry_keep(c, 0x1234, m1.s);
        CHECK(c.link == 0x1234 && c.delay == 4);
        CHECK(jpdr::carry_start(c, 0x1234, 3, p) == 4);   // same link and peer
        CHECK(jpdr::carry_start(c, 0x9999, 3, p) == 3);   // another link, room or peer
        CHECK(jpdr::carry_start(c, 0, 3, p) == 3);        // no key (no plan)
        jpdr::Params p3; p3.hi = 3; p3.clamp();
        CHECK(jpdr::carry_start(c, 0x1234, 2, p3) == 3);  // clamped to MAX
        // the carried start is the guard's base: a start of 4 may not go above 5 before 10 s (MAX 4 here anyway), and the next match begins there
        Sim m2; m2.p.clamp(); m2.cur = jpdr::carry_start(c, 0x1234, 3, m2.p); m2.rtt = [](uint64_t) { return 240.0; };
        CHECK(m2.cur == 4); CHECK(m2.run(30000) == 4); CHECK(m2.changes == 0); CHECK(m2.s.start_delay == 4);
        // a match under 20 s leaves -1: the next match uses the default
        jpdr::State short_m; uint64_t t = 1000;
        for (int i = 0; i < 300; i++, t += 33) jpdr::note_time(short_m, t, 4);
        jpdr::carry_keep(c, 0x1234, short_m);
        CHECK(c.delay == -1); CHECK(jpdr::carry_start(c, 0x1234, 3, p) == 3);
        jpdr::carry_keep(c, 0, m1.s);   // no key: nothing to carry
        CHECK(c.delay == -1 && jpdr::carry_start(c, 0, 3, p) == 3);
    }
    std::printf("net delay rule: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
