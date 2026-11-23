// Host test of Select character on the game's own Battle settings page for every player (installer/port/jsp/jsp_net_pick_page.h,
// JSP_PICK_NATIVE_SELECT=1): the switch and the target list, the read-back of the pick block, the page step's timelines (the Square only on a
// settled room page, retries, the port picker as the fallback, the lock when the player is back, the deadline), and two PCs end to end: the
// real jspf::Flow (agreement, in-the-room flags, the host's start gate, the committed pair) driven the way jsp_net_game.cpp drives it, over a
// lossy link, with a model of each PC's room page and Battle settings page. Last, a source guard on the wiring in jsp_net_game.cpp.
// No game, no GPU.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "../port/jsp/jsp_net_pick_page.h"
#include "../port/jsp/jsp_net_pick_flow.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace jsps;

// ---- 1. the switch, the targets, the read-back ----
static void test_parse()
{
    CHECK(key_on("1") && !key_on("0") && !key_on("") && !key_on(nullptr) && !key_on("11") && !key_on("1 "));
    uint32_t t[4] = { 7, 7, 7, 7 };
    CHECK(parse_targets("2B8388", t) == 1 && t[0] == 0x002B8388u && t[1] == 0 && t[3] == 0);
    CHECK(parse_targets("0x2B8388,2c2cec,0X2BCDB8", t) == 3 && t[1] == 0x002C2CECu && t[2] == 0x002BCDB8u);
    uint32_t keep[4] = { 1, 2, 3, 4 };
    CHECK(parse_targets("", keep) == 0 && keep[0] == 1);
    CHECK(parse_targets(nullptr, keep) == 0 && keep[0] == 1);
    CHECK(parse_targets("2B8388,zz", keep) == 0 && keep[0] == 1);       // a bad entry: nothing taken
    CHECK(parse_targets("1,2,3,4,5", keep) == 0 && keep[3] == 4);       // more than 4
    CHECK(parse_targets("0", keep) == 0 && parse_targets(",", keep) == 0 && parse_targets("123456789", keep) == 0);
    CHECK(parse_targets("1,2,3,4", t) == 4 && t[3] == 4);
    CHECK(kPageTg[0] == 0x002B8388u && kPageTg[1] == 0x002C2CECu && kPageTg[2] == 0x002BCDB8u);
}
static void test_read_pick()
{
    auto fok = [](int id) { return id >= 0 && id < 42 && id != 17; };   // 17: a hole in the named list
    bool sup_ids = false;
    auto sok = [&](int id) { return sup_ids && id >= 200 && id < 240; };
    uint16_t f = 0, s = 0; bool odd = true;
    read_pick(4, 0x133, fok, sok, &f, &s, &odd);
    CHECK(f == 4 && s == kRandom && !odd);
    read_pick(0x133, 0x133, fok, sok, &f, &s, &odd);
    CHECK(f == kRandom && s == kRandom && !odd);
    read_pick(17, 0x133, fok, sok, &f, &s, &odd);          // a unit the port cannot name: Random, logged
    CHECK(f == kRandom && odd);
    read_pick(0x00010004u, 0x133, fok, sok, &f, &s, &odd);   // high half not 0 (U1a: u32 with high half 0)
    CHECK(f == kRandom && odd);
    read_pick(9, 0xD2, fok, sok, &f, &s, &odd);              // Support ids off: the room's own support
    CHECK(f == 9 && s == kRandom && !odd);
    sup_ids = true;
    read_pick(9, 0xD2, fok, sok, &f, &s, &odd);
    CHECK(f == 9 && s == 0xD2 && !odd);
    read_pick(9, 0x1C, fok, sok, &f, &s, &odd);              // not a Support id: Random, not "odd"
    CHECK(s == kRandom && !odd);
}

// ---- 2. the page step on its own ----
struct Run {
    PageStep p; Cfg c; uint64_t t = 0;
    int squares = 0, locks = 0, fallbacks = 0, giveups = 0, lives = 0, retries = 0;
    void start() { p.start(t, c); }
    Out step(bool room, bool page, bool fight = false)
    {
        In in; in.now = t; in.room_now = room; in.page_now = page; in.fight_now = fight;
        const Out o = p.step(in);
        squares += o.square; locks += o.lock; fallbacks += o.fallback; giveups += o.give_up; lives += o.read_live; retries += o.retry;
        return o;
    }
};
static void test_unsaved_step()
{   // test8: the page closes with the block unchanged (Circle = Back): UNSAVED, no Square, no lock, "in the room" held (active)
    Run r; r.start();
    r.t = 0; r.step(true, false);
    r.t = 1500; CHECK(r.step(true, false).square);
    r.t = 1800; CHECK(r.step(true, true).opened);
    r.t = 6000; r.step(true, true);
    In in; in.now = 6100; in.room_now = true; in.page_now = false; in.saved = false;
    CHECK(r.p.step(in).closed && r.p.state() == St::BACK);
    in.now = 7200; Out o = r.p.step(in);
    CHECK(o.unsaved && !o.lock && r.p.state() == St::UNSAVED && r.p.unsaved() && r.p.active() && !r.p.on_page() && !r.p.finished());
    for (in.now = 7300; in.now < 60000; in.now += 100) { o = r.p.step(in); CHECK(!o.square && !o.lock && !o.unsaved && r.p.state() == St::UNSAVED); }
    // the player's own Square: open again; then Start (saved): back, lock
    in.page_now = true; o = r.p.step(in); CHECK(o.opened && r.p.state() == St::OPEN && r.p.opens() == 2);
    in.now += 5000; r.p.step(in);
    in.page_now = false; in.saved = true; in.now += 100; CHECK(r.p.step(in).closed);
    in.now += 1100; o = r.p.step(in); CHECK(o.lock && r.p.state() == St::DONE && r.p.finished());
    // a second Back on the reopened page: UNSAVED again (a notice each time)
    Run q; q.start(); q.t = 1500; q.step(true, false); q.t = 1800; q.step(true, true);
    In k; k.room_now = true; k.saved = false; k.now = 6000; k.page_now = false; q.p.step(k); k.now = 7100; CHECK(q.p.step(k).unsaved);
    k.page_now = true; k.now = 9000; CHECK(q.p.step(k).opened); k.page_now = false; k.now = 12000; q.p.step(k); k.now = 13100; CHECK(q.p.step(k).unsaved);
    // the round's idle lock while UNSAVED: DONE with that pick
    k.own_locked = true; k.now = 20000; o = q.p.step(k); CHECK(o.lock && q.p.state() == St::DONE);
    // nothing saved but the round already locked this PC's pick when the page closed: lock at once, no notice
    Run w; w.start(); w.t = 1500; w.step(true, false); w.t = 1800; w.step(true, true);
    In m; m.room_now = true; m.saved = false; m.own_locked = true; m.now = 6000; w.p.step(m); m.now = 7100; o = w.p.step(m); CHECK(o.lock && !o.unsaved);
    // UNSAVED still ends at the deadline (the player never comes back to it)
    Run z; z.c.deadline_ms = 50000; z.start(); z.t = 1500; z.step(true, false); z.t = 1800; z.step(true, true);
    In y; y.room_now = true; y.saved = false; y.now = 6000; z.p.step(y); y.now = 7100; z.p.step(y); CHECK(z.p.state() == St::UNSAVED);
    y.now = 50000; CHECK(z.p.step(y).give_up);
}
static void test_step()
{
    {   // the plain run: settle, Square, open, pick, back, lock
        Run r; r.start();
        for (; r.t < 1400; r.t += 100) { r.step(true, false); CHECK(r.squares == 0); }
        r.t = 1500; CHECK(r.step(true, false).square && r.p.state() == St::SQUARE && r.p.on_page());
        r.t = 1600; r.step(true, false); CHECK(r.p.state() == St::SQUARE);
        r.t = 1800; CHECK(r.step(true, true).opened && r.p.state() == St::OPEN);
        for (r.t = 1900; r.t < 20000; r.t += 100) { const Out o = r.step(r.t % 3000 != 0, true); CHECK(o.read_live && !o.lock); }
        CHECK(r.step(true, false).closed && r.p.state() == St::BACK && !r.p.finished());
        for (r.t += 100; r.t < 20900; r.t += 100) { r.step(true, false); CHECK(r.locks == 0); }
        r.t = 21000; CHECK(r.step(true, false).lock && r.p.state() == St::DONE && r.p.finished() && !r.p.active());
        r.t = 30000; r.step(true, true); CHECK(r.locks == 1 && r.squares == 1 && r.p.state() == St::DONE);   // done is done
    }
    {   // a notice box keeps hiding the room page: no Square until 1.5 s of an unbroken room page
        Run r; r.start();
        for (; r.t < 10000; r.t += 100) r.step((r.t / 1000) % 2 == 0, false);   // 1 s shown, 1 s hidden
        CHECK(r.squares == 0 && r.p.state() == St::WAIT);
        for (; r.t < 11600; r.t += 100) r.step(true, false);
        CHECK(r.squares == 1);
        // never a Square over a fight, never while the page already shows
        Run f; f.start();
        for (; f.t < 5000; f.t += 100) f.step(true, false, true);
        CHECK(f.squares == 0);
    }
    {   // the page never opens: Square again after 5 s, then the port picker
        Run r; r.start();
        for (; r.t <= 20000 && !r.fallbacks; r.t += 100) r.step(true, false);
        CHECK(r.squares == 2 && r.retries == 1 && r.fallbacks == 1 && r.p.state() == St::FALLBACK && r.p.finished() && !r.p.on_page());
        CHECK(r.locks == 0);
    }
    {   // the player opened the page by hand before the port's Square: no Square, the page is tracked
        Run r; r.start();
        r.t = 500; CHECK(r.step(false, true).opened && r.p.state() == St::OPEN);
        CHECK(r.squares == 0);
    }
    {   // a page that closes at once opens again, once; the second short page locks what it shows
        Run r; r.start();
        for (; r.t <= 1500; r.t += 100) r.step(true, false);
        CHECK(r.squares == 1);
        r.t = 1700; r.step(true, true);
        r.t = 2000; r.step(true, false); CHECK(r.p.state() == St::BACK);
        for (r.t = 2100; r.t <= 3000; r.t += 100) r.step(true, false);
        CHECK(r.retries == 1 && r.p.state() == St::WAIT && r.locks == 0);
        for (; r.t <= 4600; r.t += 100) r.step(true, false);
        CHECK(r.squares == 2);
        r.t = 4800; r.step(true, true);
        r.t = 5000; r.step(true, false);
        for (r.t = 5100; r.t <= 6100; r.t += 100) r.step(true, false);
        CHECK(r.locks == 1 && r.p.state() == St::DONE && r.squares == 2);
    }
    {   // a flicker of the page signal in BACK is not a close; a notice box over the room in BACK waits for the room page
        Run r; r.start();
        for (; r.t <= 1500; r.t += 100) r.step(true, false);
        r.t = 1700; r.step(true, true);
        r.t = 9000; r.step(true, false); CHECK(r.p.state() == St::BACK);
        r.t = 9500; r.step(true, true); CHECK(r.p.state() == St::OPEN);
        r.t = 9600; r.step(true, false);
        r.t = 9700; r.step(false, false);                                     // box
        for (r.t = 9800; r.t <= 15000; r.t += 100) r.step(false, false);
        CHECK(r.locks == 0 && r.p.state() == St::BACK);
        for (; r.t <= 15900; r.t += 100) r.step(true, false);
        CHECK(r.locks == 0);
        r.t = 16100; r.step(true, false);
        CHECK(r.locks == 1);
    }
    {   // sitting on the page forever: the countdown in the last 30 s, then GIVEUP exactly at the deadline (180 s)
        Run r; r.start();
        for (; r.t <= 1500; r.t += 100) r.step(true, false);
        int first_count = -1; uint64_t first_at = 0;
        for (r.t = 1600; r.t < 180000; r.t += 100) {
            r.step(true, true);
            const int s = r.p.seconds_left(r.t);
            if (s >= 0 && first_count < 0) { first_count = s; first_at = r.t; }
        }
        CHECK(first_count == 30 && first_at == 150000 && r.giveups == 0);
        r.t = 180000; CHECK(r.step(true, true).give_up && r.p.state() == St::GIVEUP && !r.p.active() && !r.p.finished());
        r.t = 180100; CHECK(!r.step(true, false).give_up && r.giveups == 1 && r.locks == 0);
        // the deadline also ends a WAIT that never saw the room page
        Run w; w.start();
        for (; w.t <= 180000; w.t += 100) w.step(false, false);
        CHECK(w.giveups == 1 && w.squares == 0);
    }
}

// ---- 3. two PCs end to end ----
// A model of one PC's game between matches: the room page and the Battle settings page. The page opens 200-600 ms after a Square on the
// room page; the player writes this PC's pick block (fighter, support) while it is open; the player closes it at close_at (0 = never).
struct Game {
    bool page = false; uint64_t open_due = 0; uint32_t blk_f = 0x133, blk_s = 0x133;
    uint64_t close_at = 0, pick_at = 0; uint32_t want_f = 0x133, want_s = 0x133; bool opened_once = false; uint64_t opened_t = 0;
    // test8: back_first = the player's first close is Circle (Back: nothing written); reopen_after > 0 = then their own Square that long later
    // (the second opening picks and closes on the same pick_at / close_at as the first), 0 = never again
    bool back_first = false, backed = false; uint64_t reopen_after = 0, reopen_due = 0; int opens = 0;
    bool room_now() const { return !page && open_due == 0; }
    void press_square(uint64_t t, std::mt19937& r) { if (room_now()) open_due = t + 200 + r() % 400; }
    void tick(uint64_t t)
    {
        if (open_due && t >= open_due) { page = true; open_due = 0; opens++; if (!opened_once || backed) { opened_once = true; opened_t = t; } }
        if (page && pick_at && t >= opened_t + pick_at && !(back_first && !backed)) { blk_f = want_f; blk_s = want_s; }
        if (page && close_at && t >= opened_t + close_at) {
            page = false;
            if (back_first && !backed) { backed = true; if (reopen_after) reopen_due = t + reopen_after; }
        }
        if (reopen_due && t >= reopen_due && room_now()) { open_due = t + 300; reopen_due = 0; }   // the player's own Square
    }
};
// One PC's after-match drive, reduced to what the page step touches in jsp_net_game.cpp's pick_frame (the to-room part is a fixed delay).
struct Pc {
    jspf::Flow f; Game g; PageStep ps; bool in_room_said = false, started = false; uint64_t room_at = 0; bool room_drive = false;
    bool start_seen_with_open_page = false; int squares = 0;
    uint32_t of = 0x133, os = 0x133; int unsaved = 0; uint64_t in_room_t = 0;   // test8: the block at the last open, notices, when "in the room" went out
};
static bool g_back_first_j = false; static uint64_t g_reopen_j = 0; static int g_unsaved_j = 0, g_opens_j = 0; static uint64_t g_in_room_j = 0;
struct Wire { uint64_t at; int to, from; std::vector<uint8_t> d; };

// Returns the committed pairs' equality; *host_started: the host's start fired (start_ok) at some point; *gave: which PC gave up.
static bool two_pcs(uint32_t seed, uint64_t close_h, uint64_t close_j, uint32_t fh, uint32_t fj, bool lossy, bool* host_started, int* gave,
                    jspf::PairOut* out_h, jspf::PairOut* out_j, uint64_t* start_t, uint64_t* last_close_t)
{
    std::mt19937 r(seed);
    Pc pc[2];
    uint8_t slots[2] = { 0, 1 };
    jspg::Config pcf; jspk::Config kcf;   // the defaults: 120 s idle lock, 60 s wait
    for (int i = 0; i < 2; i++) { if (!pc[i].f.open(0x1234567890ull, i, 2, slots)) return false; pc[i].f.set_config(pcf, kcf); }
    pc[0].g.close_at = close_h; pc[1].g.close_at = close_j;
    pc[1].g.back_first = g_back_first_j; pc[1].g.reopen_after = g_reopen_j;
    pc[0].g.want_f = fh; pc[1].g.want_f = fj; pc[0].g.pick_at = close_h ? close_h / 2 : 5000; pc[1].g.pick_at = close_j ? close_j / 2 : 5000;
    std::vector<Wire> q;
    *host_started = false; *gave = -1; *start_t = 0; *last_close_t = 0;
    for (int i = 0; i < 2; i++) { pc[i].f.match_over(3, 0); pc[i].f.choose(jspg::CHARSELECT, 10 + i); }
    Cfg cfg; cfg.deadline_ms = (uint64_t)kcf.idle_ms + kcf.wait_ms;
    for (uint64_t t = 0; t <= 400000; t += 20) {
        // the link
        for (size_t k = 0; k < q.size();) {
            if (q[k].at <= t) { pc[q[k].to].f.route(q[k].from, q[k].d.data(), q[k].d.size(), t); q.erase(q.begin() + (long)k); }
            else k++;
        }
        if (t % 100 == 0)
            for (int i = 0; i < 2; i++)
                pc[i].f.pump(t, [&](const uint8_t* b, size_t n) {
                    if (lossy && r() % 5 == 0) return;
                    const uint64_t lat = lossy ? 30 + r() % 120 : 40;
                    q.push_back({ t + lat, 1 - i, i, std::vector<uint8_t>(b, b + n) });
                    if (lossy && r() % 8 == 0) q.push_back({ t + lat + r() % 200, 1 - i, i, std::vector<uint8_t>(b, b + n) });
                });
        for (int i = 0; i < 2; i++) {
            Pc& p = pc[i];
            if (*gave == i) continue;
            p.g.tick(t);
            const jspf::Act a = p.f.poll(t);
            if (a == jspf::Act::CharSelect) { p.ps.start(t, cfg); p.room_at = t + 4000 + r() % 3000; }   // the results screens, then the room
            if (a == jspf::Act::Lost) { if (*gave < 0) *gave = 10 + i; }
            if (!p.room_drive && p.room_at && t >= p.room_at) { p.room_drive = true; if (!p.ps.active()) p.f.in_room(true); }
            if (p.room_drive && p.ps.active()) {
                In in; in.now = t; in.room_now = p.g.room_now(); in.page_now = p.g.page;
                in.saved = p.g.blk_f != p.of || p.g.blk_s != p.os;   // as jsp_net_game.cpp: the block against the last open
                in.own_locked = !p.f.picking() || p.f.round().locked_of(p.f.round().me());
                const Out o = p.ps.step(in);
                if (o.opened) { p.of = p.g.blk_f; p.os = p.g.blk_s; }
                if (o.unsaved) p.unsaved++;
                auto pick = [&] { jspk::Pick k; bool odd; read_pick(p.g.blk_f, p.g.blk_s, [](int id) { return id >= 0 && id < 42; }, [](int) { return false; }, &k.fighter, &k.support, &odd); return k; };
                if (o.square) { p.g.press_square(t, r); p.squares++; }
                if (o.read_live && p.f.picking()) p.f.submit(pick(), false);
                if (o.lock) { if (p.f.picking()) p.f.submit(pick(), true); p.f.in_room(true); p.in_room_t = t; if (t > *last_close_t) *last_close_t = t; }
                if (o.fallback) p.f.in_room(true);
                if (o.give_up) { *gave = i; }
            }
            // the host's start (jsp_net_game.cpp: Room -> Start once start_ok); the start must never come while any page is up
            if (i == 0 && p.f.start_ok() && !*host_started) {
                *host_started = true; *start_t = t;
                for (int k = 0; k < 2; k++) if (pc[k].g.page || pc[k].ps.on_page() || pc[k].ps.state() == St::WAIT) pc[0].start_seen_with_open_page = true;
            }
        }
        if (*host_started && t > *start_t + 2000) break;
        if (*gave >= 0 && *gave < 10) break;   // a PC gave up: its link closes (jsp_net_game.cpp native_close); the other then loses the round
    }
    g_unsaved_j = pc[1].unsaved; g_opens_j = pc[1].g.opens; g_in_room_j = pc[1].in_room_t;
    if (pc[0].start_seen_with_open_page) { std::printf("FAIL: the host's start came while a page was up (seed %u)\n", seed); fails++; }
    const bool a = pc[0].f.pair(out_h), b = pc[1].f.pair(out_j);
    if (!a || !b) return false;
    if (out_h->n != out_j->n) return false;
    for (int i = 0; i < out_h->n; i++) if (!(out_h->pick[i] == out_j->pick[i]) || out_h->slot[i] != out_j->slot[i]) return false;
    return true;
}
static void test_two_pcs()
{
    int agreed = 0, started = 0;
    for (uint32_t seed = 1; seed <= 60; seed++) {
        const bool lossy = seed % 2 == 0;
        const uint64_t ch = 8000 + seed * 431 % 30000, cj = 6000 + seed * 977 % 50000;   // both close their pages at different times
        bool hs; int gave; jspf::PairOut h, j; uint64_t st, lc;
        const bool eq = two_pcs(seed, ch, cj, 4, 9, lossy, &hs, &gave, &h, &j, &st, &lc);
        CHECK(eq); agreed += eq;
        CHECK(hs && gave < 0); started += hs;
        if (eq) { CHECK(h.pick[0].fighter == 4 && h.pick[1].fighter == 9 && h.pick[0].support == jspk::kRandom); }
        CHECK(st >= lc);   // the start only after the last page closed and its pick was locked
    }
    std::printf("two PCs: %d of 60 runs agreed on the page picks, %d started\n", agreed, started);
    {   // the joiner sits on its page: the round's idle lock takes what its page shows, the host never starts, the joiner gives up at 180 s
        bool hs; int gave; jspf::PairOut h, j; uint64_t st, lc;
        two_pcs(99, 9000, 0, 4, 9, false, &hs, &gave, &h, &j, &st, &lc);
        CHECK(!hs && gave == 1);
    }
    {   // the host sits on its page: the same on the other side
        bool hs; int gave; jspf::PairOut h, j; uint64_t st, lc;
        two_pcs(98, 0, 7000, 4, 9, true, &hs, &gave, &h, &j, &st, &lc);
        CHECK(!hs && gave == 0);
    }
    {   // test8: the joiner backs out with Circle first (nothing saved), opens the page again with its own Square 5 s later, picks and closes:
        // one "not saved" notice, no automatic Square, "in the room" only after the second close, the host's start after that, its pick counts
        for (uint32_t seed = 200; seed < 220; seed++) {
            g_back_first_j = true; g_reopen_j = 5000;
            bool hs; int gave; jspf::PairOut h, j; uint64_t st, lc;
            const bool eq = two_pcs(seed, 9000, 7000, 4, 9, seed % 2 == 0, &hs, &gave, &h, &j, &st, &lc);
            g_back_first_j = false; g_reopen_j = 0;
            CHECK(eq && hs && gave < 0);
            CHECK(g_unsaved_j == 1 && g_opens_j == 2);
            CHECK(eq && h.pick[1].fighter == 9 && j.pick[1].fighter == 9);
            CHECK(g_in_room_j > 7000 + 5000 && st >= g_in_room_j);   // "in the room" waited for the second opening; the start waited for it
        }
    }
    {   // test8: the joiner backs out with Circle and never opens the page again: the round's idle lock takes Random (120 s), then the start
        g_back_first_j = true; g_reopen_j = 0;
        bool hs; int gave; jspf::PairOut h, j; uint64_t st, lc;
        const bool eq = two_pcs(230, 9000, 7000, 4, 9, false, &hs, &gave, &h, &j, &st, &lc);
        g_back_first_j = false;
        CHECK(eq && hs && gave < 0 && g_unsaved_j == 1 && g_opens_j == 1);
        CHECK(eq && h.pick[1].fighter == jspk::kRandom && h.pick[0].fighter == 4);
        CHECK(st >= 120000);
    }
    {   // a joiner that picks nothing the port knows (a unit outside the named list) plays Random; both PCs agree
        bool hs; int gave; jspf::PairOut h, j; uint64_t st, lc;
        CHECK(two_pcs(97, 9000, 12000, 4, 0x2D, true, &hs, &gave, &h, &j, &st, &lc));
        CHECK(hs && h.pick[1].fighter == jspk::kRandom && j.pick[1].fighter == jspk::kRandom);
    }
}

// ---- 4. source guard: the wiring in jsp_net_game.cpp ----
static std::string slurp(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void test_source_guard(const char* dir)
{
    const std::string g = slurp((std::string(dir) + "/../port/jsp/jsp_net_game.cpp").c_str());
    const std::string n = slurp((std::string(dir) + "/../port/jsp/jsp_net_native.cpp").c_str());
    if (g.empty() || n.empty()) { std::printf("FAIL: sources not found from %s\n", dir); fails++; return; }
    // the "in the room" flag waits for the page step (the host's start waits for every page)
    CHECK(g.find("pk_drive(PkDrive::Room); if (!u.ps.active()) f.in_room(true);") != std::string::npos);
    // the lock and the fallback say "in the room"; the give-up closes the link without giving the room up
    CHECK(g.find("pick locked from the Battle settings page") != std::string::npos);
    CHECK(g.find("if (g_nat.link) native_close(\"post-game: still on the Battle settings page at the round's deadline\");") != std::string::npos);
    // no Ready Cross and no notice-box Cross while the page step runs
    CHECK(g.find("u.drive == PkDrive::Room && !u.ps.active(), host, picking, room_now") != std::string::npos);
    CHECK(g.find("|| u.ps.on_page() ||") != std::string::npos);
    // the switch: exactly "1", default off; it replaces the host-only page
    CHECK(g.find("jsp_setting_get(jsps::kKey, b, sizeof b) && jsps::key_on(b)") != std::string::npos);
    CHECK(g.find("native_page = native_sel ? 0 : native_page_set") != std::string::npos);
    // the page signal: the page's update targets, exported by jsp_net_native.cpp only while the pick switch is on
    CHECK(n.find("for (uint32_t k : g_page_tg) if (k && target == k) g_page_ms = t;") != std::string::npos);
    CHECK(n.find("jsp_native_page_seen_ms(void) { return g_pick ? g_page_ms.load() : 0ull; }") != std::string::npos);
}

int main(int argc, char** argv)
{
    test_parse();
    test_read_pick();
    test_step();
    test_unsaved_step();
    test_two_pcs();
    std::string dir = argc > 0 ? argv[0] : ".";
    if (argc > 1) dir = argv[1];
    test_source_guard(dir.c_str());
    if (fails) { std::printf("net_pick_page_test: %d FAILED\n", fails); return 1; }
    std::printf("net_pick_page_test: all passed\n");
    return 0;
}
