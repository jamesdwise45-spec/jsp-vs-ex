// Host test (FIX pick7) of the after-match menu against the per-match rules it sits between, on the test6 line: the match-end rule shared
// by the share pad watchdog's flag and the prompt (jspo::end_rule over jkr::normal_end, jsp_net_ko_rule.h), the watchdog's late-but-alive stop
// (jsp_sharepad_wd.h), the start of the input delay rule (jsp_net_delay_rule.h) and the [perf] summary (jsp_net_perf.h) across Fight again
// hand-offs. jsp_net_game.cpp resets the delay rule's and the perf summary's state per match in reset_match (end_with -> reset_match);
// net_pick_match_test.sh checks those lines in the source, and this test shows what each reset is for: without it the second match after a menu
// decides its input delay from the previous match's round trips at its sync and logs the menu as a hitch. No game, no GPU.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "../port/jsp/jsp_net_delay_rule.h"
#include "../port/jsp/jsp_net_perf.h"
#include "../port/jsp/jsp_sharepad_wd.h"
// glibc's <math.h> declares a function jnf (Bessel); the port's namespace of the same name is renamed for this test only (the math headers are in above)
#define jnf jsp_native_fight_ns
#include "../port/jsp/jsp_net_pick_own.h"
#include "../port/jsp/jsp_net_pick_flow.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ---- 1. the match-end rule ----
static void test_end_rule()
{
    for (int w = 1; w <= 3; w++) {
        const jspo::End e = jspo::end_rule(jkr::match_over_text(w));
        CHECK(e.wd_flag && e.prompt && e.last_end_ok);
        const jspo::End k = jspo::end_rule(jkr::disconnect_end_text(w));   // a confirmed KO kept across a disconnect: a normal end
        CHECK(k.wd_flag && k.prompt && k.last_end_ok);
    }
    const jspo::End d = jspo::end_rule(jkr::disconnect_end_text(0));
    CHECK(!d.wd_flag && !d.prompt && !d.last_end_ok);
    const jspo::End f9 = jspo::end_rule("Match ended");   // F9 / Cancel: no prompt, no watchdog, but not a bad end for the re-arm
    CHECK(!f9.wd_flag && !f9.prompt && f9.last_end_ok);
    for (const char* t : { "Rollback session could not start: x", "Desync at tick 120", "Opponent disconnected", "match cancelled: x", "", "Match", "match over: red wins" }) {
        const jspo::End e = jspo::end_rule(t);
        CHECK(!e.wd_flag && !e.prompt && !e.last_end_ok);
    }
}

// ---- 2. Fight again hand-offs: the watchdog's flag and the prompt, match after match ----
// A model of jsp_net_native_fight.cpp's flag (g_end_normal) and scope (g_wd_after): arm(1) clears both; jsp_native_fight_end_normal() sets the
// flag; arm(0) consumes the flag and, only when it was armed, opens the scope on a normal end; the mode's free closes the scope.
struct Wd {
    int end_normal = 0, armed = 0, after = 0;
    void arm() { armed = 1; end_normal = 0; after = 0; }
    void set_normal() { end_normal = 1; }
    void disarm() { const int n = end_normal; end_normal = 0; if (armed) after = n; armed = 0; }
    void mode_free() { after = 0; }
};
static void test_handoffs()
{
    const uint8_t slots[2] = { 0, 1 };
    jspf::Flow f[2];
    for (int i = 0; i < 2; i++) CHECK(f[i].open(777, i, 2, slots));
    uint64_t t = 0;
    auto step = [&] {
        f[0].pump(t, [&](const uint8_t* d, size_t n) { f[1].route(0, d, n, t); });
        f[1].pump(t, [&](const uint8_t* d, size_t n) { f[0].route(1, d, n, t); });
        f[0].poll(t); f[1].poll(t); t += 100;
    };
    for (int k = 0; k < 30; k++) step();
    CHECK(f[0].agreed() && f[1].agreed());
    Wd wd[2];
    const std::string ends[] = { jkr::match_over_text(1), jkr::match_over_text(2), jkr::disconnect_end_text(0), jkr::match_over_text(3), "Match ended",
                                 jkr::disconnect_end_text(2), "Desync at tick 99", jkr::match_over_text(1), jkr::match_over_text(2), jkr::match_over_text(1) };
    int prompts = 0;
    for (uint16_t k = 1; k <= (uint16_t)(sizeof ends / sizeof ends[0]); k++) {
        const std::string& text = ends[k - 1];
        for (int i = 0; i < 2; i++) {
            wd[i].arm();                                  // start_session: jsp_native_fight_arm(1)
            CHECK(!wd[i].after && !wd[i].end_normal);     // the scope of the previous exit never reaches into a fight
            f[i].fight_running();
        }
        for (int i = 0; i < 2; i++) {                     // end_with, in its order
            const jspo::End er = jspo::end_rule(text);
            if (er.wd_flag) wd[i].set_normal();
            wd[i].disarm();
            const bool open = er.prompt && f[i].match_end(k, t);
            wd[i].disarm();                               // reset_match disarms again: a no-op for the scope
            CHECK(open == er.prompt);                     // agreed PCs: the prompt opens exactly on a normal end ...
            CHECK(!open || wd[i].after);                  // ... and every open prompt has the watchdog armed for the exit under it
            CHECK(wd[i].after == (er.wd_flag ? 1 : 0) && !wd[i].end_normal);   // the flag never leaks to a later disarm
            CHECK(f[i].hold() == open);
            prompts += open;
        }
        if (f[0].hold()) {   // Fight again on both, the room is reached, the round is READY: the next match may arm
            CHECK(f[0].choose(jspg::REMATCH, t) && f[1].choose(jspg::REMATCH, t));
            for (int s = 0; s < 40 && (f[0].hold() || f[1].hold()); s++) step();
            CHECK(!f[0].hold() && !f[1].hold());
        }
        for (int i = 0; i < 2; i++) wd[i].mode_free();    // the game's teardown on the way back to the room
    }
    CHECK(prompts == 2 * 7);
}

// ---- 2b. the watchdog's late-but-alive stop under the menu, match after match ----
// The share pad watchdog (jsp_sharepad_wd.h) feeds a starved peer member after a normal end and stops that member for the episode once a REAL
// sample from it arrives after a feed (State::stopped). The menu holds the game between the disarm and the mode's free, which is that episode:
// a stop must hold for the rest of the menu, and must not carry into the next match's exit (the next arm starts a new episode: jspw::reset, as
// wd_tick does on a new g_wd_epoch). The neutral samples fed before the stop stay queued until the next online mode's build resets the queues.
static jspw::View starved_view(uint32_t real)
{
    jspw::View v;
    v.guards = true; v.paused = true; v.active = 1; v.sync = jspw::kStateStarved; v.members = 2; v.own = 2;
    v.st[0] = jspw::kStateNormal; v.count[0] = 2; v.queue_ok[0] = true;   // this PC's own member
    v.st[1] = jspw::kStateStarved; v.count[1] = 0; v.queue_ok[1] = true;  // the peer, starved
    v.real = real;
    return v;
}
static void test_wd_stop_per_match()
{
    jspw::State s;
    for (int m = 0; m < 3; m++) {
        jspw::reset(s);   // start_session: arm(1) -> a new episode
        uint32_t fed = 0;
        for (uint32_t k = 0; k < jspw::kStartTicks + 3; k++) fed += jspw::step(s, starved_view(0)) >> 1 & 1;
        CHECK(fed == 4 && !s.stopped[1]);                    // starved under the prompt: fed from the 10th tick, as without the menu
        CHECK(jspw::step(s, starved_view(2u)) == 0);         // the peer is late but alive: its real sample stops the feed ...
        CHECK(s.stopped[1] && s.surplus[1] == 4);
        for (int k = 0; k < 600; k++) CHECK(jspw::step(s, starved_view(0)) == 0);   // ... for the rest of the menu (20 s starved again: never fed)
    }
}

// ---- 3. the input delay rule starts afresh in every match ----
// A 200 ms link: rule 2 with the shipped Params. One match = 20 s from its sync, stepped every 100 ms (adapt_delay's poll), the session's delay
// starting at the manifest's 2 (a new session per match). Returns the time from the sync of the first raise (to 3), or -1. With the state reset
// per match the raise comes from this match's own round trips: after the sync and by the first reading (first_ms + every_ms). This holds on
// final2/test6 (the first reading at 4 s, guarded) and with the delay follow-up's slow-link start (about 1 s), so it does not depend on it.
static int64_t run_match(jpdr::State& s, const jpdr::Params& p, uint64_t sync)
{
    int cur = 2; uint32_t seq = 0; int64_t raise_at = -1;
    for (uint64_t now = sync; now < sync + 20000; now += 100) {
        if ((now - sync) % 500 == 0) seq++;   // GekkoNet pings every 500 ms
        const jpdr::Decision d = jpdr::step(s, p, now, [&] { jpdr::Reading r; r.ping_avg = 200; r.jitter = 2; r.last_ping = 200; r.rtt_seq = seq; r.cur = cur; return r; });
        if (d.want >= 0 && d.want != d.cur) {
            if (raise_at < 0 && d.want > d.cur) raise_at = (int64_t)(now - sync);
            cur = d.want;
        }
    }
    return raise_at;
}
static void test_delay_start_per_match()
{
    jpdr::Params p;
    jpdr::State s;
    uint64_t sync = 100000;
    for (int m = 0; m < 3; m++) {
        s = jpdr::State();   // reset_match: g_ad = jpdr::State()
        const int64_t at = run_match(s, p, sync);
        CHECK(at > 0 && at <= (int64_t)(p.first_ms + p.every_ms));   // every match raises from its own round trips, by its first reading
        sync += 20000 + 90000;          // the after-match menu and the room: 90 s
    }
    // without the reset the second match decides at its sync, from the first match's round trips, before it measured any of its own
    jpdr::State kept;
    CHECK(run_match(kept, p, 100000) > 0);
    CHECK(run_match(kept, p, 210000) == 0);
}

// ---- 4. the [perf] lines are per match ----
static void test_perf_per_match()
{
    jpperf::Match m;
    double now = 0;
    for (uint32_t k = 1; k <= 3; k++) {
        m = jpperf::Match();   // reset_match: g_perf = jpperf::Match()
        for (int i = 0; i < 300; i++, now += 33.3) jpperf::on_frame(m, now, true, 2);
        CHECK(m.hitch.n == 0 && m.frames == 300);
        const std::string s = jpperf::lines(m, "direct", "re-psn", "host", k, 10.0);
        CHECK(s.find("[perf] match " + std::to_string(k) + ":") == 0 && s.find("[perf] hitches: 0 gaps") != std::string::npos);
        now += 90000;   // the menu and the room: no frames of a match
    }
    // without the reset the menu's 90 s shows up as one hitch of the next match
    jpperf::Match kept;
    now = 0;
    for (int i = 0; i < 300; i++, now += 33.3) jpperf::on_frame(kept, now, true, 2);
    now += 90000;
    for (int i = 0; i < 300; i++, now += 33.3) jpperf::on_frame(kept, now, true, 2);
    CHECK(kept.hitch.n == 1 && kept.hitch.max > 89000);
}

int main()
{
    test_end_rule();
    test_handoffs();
    test_wd_stop_per_match();
    test_delay_start_per_match();
    test_perf_per_match();
    if (fails) { std::printf("net pick match: %d check(s) FAILED\n", fails); return 1; }
    std::printf("net pick match: end rule (watchdog flag = prompt = jkr::normal_end), 10 hand-offs with the watchdog scope, its late-but-alive stop, the delay rule's start and [perf] per match ok\n");
    return 0;
}
