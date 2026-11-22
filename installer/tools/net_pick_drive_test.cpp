// Host test of the after-match drive rules (installer/port/jsp/jsp_net_pick_drive.h): the prompt's pad gate (capture, the host's late-pick start
// block, injected presses), the notice-box Cross of the mapping run's U4 fix (map1: "You got a new title." over the room), and the joiner's Ready
// Cross (U7). It also replays the two map1 timelines on a model room page, with the host's start block on, to show the box always closes and the
// start never runs before every pick is committed. No game, no GPU.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include "../port/jsp/jsp_net_pick_drive.h"
#include "../port/jsp/jsp_net_pick_flow.h"      // apply_pair: the roster write at the builder (ownership after Select character)
// glibc's <math.h> declares a function jnf (Bessel); the port's namespace of the same name is renamed for this test only (the math headers are in above)
#define jnf jsp_native_fight_ns
#include "../port/jsp/jsp_net_native_fight.h"   // jnf::placement_net_slot: the builder's net slot of a record (swapped-controls fix)
#include "../port/jsp/jsp_net_pick_own.h"       // FIX (pick7): the planned seats and the fighter map the session start builds (jnf::net_ports)

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace jspd;

// ---- 1. the pad gate ----
static void test_pad_gate()
{
    uint16_t prev = 0;
    // free: the player's buttons pass; an injected press is OR-ed in only while live
    PadOut o = pad_gate(kBtnUp, &prev, false, false, kBtnCross, false);
    CHECK(o.buttons == kBtnUp && !o.centre && o.edges == kBtnUp);
    o = pad_gate(kBtnUp, &prev, false, false, kBtnCross, true);
    CHECK(o.buttons == (kBtnUp | kBtnCross) && o.edges == 0);   // held: no new edge
    // capture: the game reads a released pad; the prompt gets each new press once
    prev = 0;
    o = pad_gate(kBtnCross, &prev, true, false, kBtnCross, true);
    CHECK(o.buttons == 0 && o.centre && o.edges == kBtnCross);
    o = pad_gate(kBtnCross, &prev, true, false, 0, false);
    CHECK(o.buttons == 0 && o.edges == 0);
    o = pad_gate(kBtnCross | kBtnDown, &prev, true, false, 0, false);
    CHECK(o.buttons == 0 && o.edges == kBtnDown);
    // capture wins over block
    o = pad_gate(kBtnCircle, &prev, true, true, 0, false);
    CHECK(o.buttons == 0 && o.centre && o.edges == kBtnCircle);
    // late-pick block (the host's start): the player's own Cross is swallowed, other buttons pass, and NO injected press goes through
    prev = 0;
    o = pad_gate(kBtnCross | kBtnSquare, &prev, false, true, kBtnCross, true);
    CHECK(o.buttons == kBtnSquare && !o.centre && o.edges == (kBtnCross | kBtnSquare));
    o = pad_gate(0, &prev, false, true, kBtnCross, true);
    CHECK(o.buttons == 0);
    o = pad_gate(kBtnCircle, &prev, false, true, kBtnCross, true);
    CHECK(o.buttons == kBtnCircle);   // Leave stays possible in the room (Circle) while the start is blocked
    // block lifted: the injected Cross reaches the game again
    o = pad_gate(0, &prev, false, false, kBtnCross, true);
    CHECK(o.buttons == kBtnCross);
}

// ---- 2. the notice-box Cross ----
static BoxIn reached_hidden(uint64_t room_since)
{
    BoxIn in; in.reached = true; in.room_now = false; in.room_since = room_since; return in;
}
static void test_box_basic()
{
    BoxCross b; int back = 0;
    BoxIn in;   // not reached: never
    for (uint64_t t = 1000; t < 20000; t += 100) CHECK(b.step(in, t, 0, &back) == BoxCross::NONE);
    // reached and the room shows: nothing
    in.reached = true; in.room_now = true; in.room_since = 500;
    for (uint64_t t = 20000; t < 21000; t += 100) CHECK(b.step(in, t, 0, &back) == BoxCross::NONE);
    // hidden: the first Cross 2.5 s after the page went away, then one a second at most, 8 in a row
    in = reached_hidden(500);
    uint64_t press_t = 0, first = 0; int n = 0;
    for (uint64_t t = 21000; t < 40000; t += 50) {
        if (b.step(in, t, press_t, &back) == BoxCross::PRESS) {
            if (!n) first = t;
            else CHECK(t - press_t >= BoxCross::kGapMs);
            press_t = t; n++;
        }
    }
    CHECK(first == 21000 + BoxCross::kHiddenMs);
    CHECK(n == BoxCross::kMax && b.away_n == BoxCross::kMax);
    // the room comes back: BACK with the count, then the counters start over
    in.room_now = true; back = 0;
    CHECK(b.step(in, 40000, press_t, &back) == BoxCross::BACK && back == BoxCross::kMax);
    CHECK(b.step(in, 40050, press_t, &back) == BoxCross::NONE && b.away_n == 0 && b.away_since == 0);
    in.room_now = false; n = 0;
    for (uint64_t t = 41000; t < 60000; t += 50) if (b.step(in, t, press_t, &back) == BoxCross::PRESS) { press_t = t; n++; }
    CHECK(n == BoxCross::kMax);
}
static void test_box_gates()
{
    int back = 0;
    {   // capture (the picker holds the pad) and the player's own page: no Cross, but the hidden time keeps counting
        BoxCross b; BoxIn in = reached_hidden(100); in.capture = true;
        for (uint64_t t = 1000; t < 10000; t += 50) CHECK(b.step(in, t, 0, &back) == BoxCross::NONE);
        in.capture = false; in.own_page = true;
        for (uint64_t t = 10000; t < 20000; t += 50) CHECK(b.step(in, t, 0, &back) == BoxCross::NONE);
        in.own_page = false;
        CHECK(b.step(in, 20000, 0, &back) == BoxCross::PRESS);   // at once: hidden long enough
    }
    {   // a fight drawn resets (the next fight started)
        BoxCross b; BoxIn in = reached_hidden(100);
        CHECK(b.step(in, 1000, 0, &back) == BoxCross::NONE);
        CHECK(b.step(in, 3600, 0, &back) == BoxCross::PRESS);
        in.fight_now = true;
        CHECK(b.step(in, 3700, 3600, &back) == BoxCross::BACK && back == 1);
        CHECK(b.step(in, 9000, 3600, &back) == BoxCross::NONE);
    }
    {   // the online loading scene ran after the room was reached: no more Crosses this round, even with the page hidden for long
        BoxCross b; BoxIn in = reached_hidden(1000); in.loading_seen = 5000;
        for (uint64_t t = 6000; t < 30000; t += 50) CHECK(b.step(in, t, 0, &back) == BoxCross::NONE);
        CHECK(b.loading);
        in.reached = false;   // a new drive: loading forgotten
        CHECK(b.step(in, 30000, 0, &back) == BoxCross::NONE && !b.loading);
        in = reached_hidden(31000); in.loading_seen = 5000;   // a loading seen BEFORE this room does not count
        CHECK(b.step(in, 31000, 0, &back) == BoxCross::NONE);
        CHECK(b.step(in, 31000 + BoxCross::kHiddenMs, 0, &back) == BoxCross::PRESS);
    }
    {   // the drive's other presses count for the 1 s gap
        BoxCross b; BoxIn in = reached_hidden(100);
        CHECK(b.step(in, 1000, 0, &back) == BoxCross::NONE);
        CHECK(b.step(in, 3600, 3000, &back) == BoxCross::NONE);
        CHECK(b.step(in, 4000, 3000, &back) == BoxCross::PRESS);
    }
}

// ---- 3. the joiner's Ready Cross (U7) ----
static void test_join_ready()
{
    bool j = false;
    CHECK(!join_ready(0, true, false, false, true, &j, 1000, 0) && !j);   // JSP_PICK_JOIN_READY=0: never
    CHECK(!join_ready(1, true, true, false, true, &j, 1000, 0));          // the host never
    CHECK(!join_ready(1, true, false, true, true, &j, 1000, 0));          // not while picking
    CHECK(!join_ready(1, true, false, false, false, &j, 1000, 0));        // not with the room page hidden
    CHECK(!join_ready(1, false, false, false, true, &j, 1000, 0));        // not outside the "in the room" drive
    CHECK(join_ready(1, true, false, false, true, &j, 1000, 0) && j);     // once
    for (uint64_t t = 1000; t < 60000; t += 100) CHECK(!join_ready(1, true, false, false, true, &j, t, 1000));
    j = false; int n = 0; uint64_t pt = 0;
    for (uint64_t t = 1000; t < 21100; t += 100) if (join_ready(2, true, false, false, true, &j, t, pt)) { pt = t; n++; }
    CHECK(n == 5);   // mode 2: at once, then every 5 s (1, 6, 11, 16, 21 s)
}

// ---- 4. map1 timelines on a model room page ----
// The model: the room page is up from `arrive`; a notice box opens `box_after` ms later and hides it until a Cross reaches the game (the game
// reads the pad each 33 ms frame; an injected press is live for 120 ms). block_until: the host's start block (Flow::block_start) is on until then.
struct Run { int crosses_to_game = 0; uint64_t box_closed = 0; };
static Run run_box(uint64_t arrive, uint64_t box_after, uint64_t block_until, uint64_t end, bool with_gate = true)
{
    Run r; BoxCross b; uint16_t prev = 0; uint64_t press_t = 0, press_until = 0; bool box = false, box_done = false, was_down = false;
    for (uint64_t t = arrive; t < end; t += 33) {
        if (!box_done && !box && t >= arrive + box_after) box = true;
        BoxIn in; in.reached = true; in.room_now = !box; in.room_since = arrive; in.blocked = with_gate && t < block_until;
        int back = 0;
        if (b.step(in, t, press_t, &back) == BoxCross::PRESS) { press_t = t; press_until = t + 120; }
        const PadOut o = pad_gate(0, &prev, false, t < block_until, kBtnCross, t < press_until);
        const bool down = (o.buttons & kBtnCross) != 0;
        if (down && !was_down) { r.crosses_to_game++; if (box) { box = false; box_done = true; r.box_closed = t; } }   // the game acts on a new press
        was_down = down;
    }
    return r;
}
static void test_map1()
{
    // the joiner after match 1: no block (joiners are never blocked): the box closes 2.5 s after it opened
    Run j = run_box(10000, 1200, 0, 80000);
    CHECK(j.box_closed && j.box_closed - (10000 + 1200) <= BoxCross::kHiddenMs + 100);
    CHECK(j.crosses_to_game == 1);   // one Cross, and none after the box closed (the room page is back)
    // the host after match 2 (the 60 s stall of map1): the host's block holds its Crosses back until every pick is committed (here 20 s later),
    // then the box closes within the gap; no Cross reached the game while blocked, so "Go to battle" could not be pressed early
    Run h = run_box(10000, 1200, 30000, 80000);
    CHECK(h.box_closed >= 30000 && h.box_closed <= 30000 + 100 && h.crosses_to_game == 1);
    Run hold = run_box(10000, 1200, 30000, 80000, false);   // the same without the blocked gate: all 8 Crosses swallowed, the box stays
    CHECK(hold.box_closed == 0 && hold.crosses_to_game == 0);
    // a block longer than 8 Crosses' worth (a long Select character round): without the blocked gate (the code as it was in the 4-player tree) the
    // block swallowed all 8 Crosses and the box stayed (the host's start presses need the room page, so only the 60 s start timeout and the
    // player's own Cross got out of it); with the gate the Crosses wait for the block and the box closes right after it lifts
    Run h2old = run_box(10000, 1200, 60000, 80000, false);
    CHECK(h2old.box_closed == 0 && h2old.crosses_to_game == 0);
    Run h2 = run_box(10000, 1200, 60000, 80000);
    CHECK(h2.box_closed >= 60000 && h2.box_closed <= 60000 + 100 && h2.crosses_to_game == 1);
}

// ---- 5. ownership after Select character: own pad -> own fighter ----
// Two PCs (host = room record 0 / member 1, joiner = record 1 / member 2, as mapping run U5 found), random room images in which either PC's
// versus_apply may or may not have swapped team bytes, random picks and both team modes. After the roster write on each PC: the two images are
// equal (except each PC's own pick block), and the builder's placement gives every human the net slot equal to its record = its rollback slot
// (host net 0 / joiner net 1), so the pad take needs no remap and each PC's pad drives its own fighter. The game must still show it: PLAN.md.
static void test_ownership()
{
    std::mt19937 r(20261122);
    int runs = 0;
    for (int it = 0; it < 2000; it++) {
        uint8_t img[jspk::kRoomSize];
        for (uint32_t i = 0; i < jspk::kRoomSize; i++) img[i] = (uint8_t)r();
        for (int i = 0; i < 4; i++) img[jspk::kOffSlotTeam + i] = (uint8_t)(r() % 3);
        const int t[4] = { 0, 0, 1, 1 };
        for (int rc = 0; rc < 4; rc++) {
            uint8_t* p = img + jspk::kOffRecords + jspk::kRecSize * rc;
            const bool human = rc < 2;
            jspk::put_be16(p + jspk::kRecMember, human ? (uint16_t)(rc + 1) : 0); p[jspk::kRecCom] = human ? 0 : 1;
            p[jspk::kRecTeam] = (uint8_t)(r() % 4 == 0 ? (t[rc] ^ 1) : t[rc]);   // a PC's versus_apply may have moved a record already
            jspk::put_be16(p + jspk::kRecFighter, (uint16_t)(r() % 42)); jspk::put_be16(p + jspk::kRecSupport, (uint16_t)(200 + r() % 14));
        }
        uint8_t pc[2][jspk::kRoomSize];
        std::memcpy(pc[0], img, sizeof img); std::memcpy(pc[1], img, sizeof img);
        if (r() & 1) std::swap(pc[1][jspk::kOffRecords + jspk::kRecSize * 1 + jspk::kRecTeam], pc[1][jspk::kOffRecords + jspk::kRecSize * 3 + jspk::kRecTeam]);   // only the joiner's PC swapped
        jspf::PairOut p; p.n = 2; p.slot[0] = 0; p.slot[1] = 1;
        for (int k = 0; k < 2; k++) { p.pick[k].fighter = r() % 3 == 0 ? jspk::kRandom : (uint16_t)(r() % 42); p.pick[k].support = jspk::kRandom; }
        p.pick[0].mode = (r() & 1) ? jspk::TOGETHER : jspk::VERSUS;   // the host's mode wins
        p.pick[1].mode = (r() & 1) ? jspk::TOGETHER : jspk::VERSUS;
        bool ok = true;
        for (int me = 0; me < 2; me++) { p.self_slot = (uint8_t)me; ok = ok && jspf::apply_pair(pc[me], p) == jspk::Result::Ok; }
        CHECK(ok);
        if (!ok) continue;
        for (uint32_t i = 0; i < jspk::kRoomSize; i++)
            if (i < jspk::kOffSelfFighter || i >= jspk::kOffSelfFighter + 4) if (i < jspk::kOffSelfSupport || i >= jspk::kOffSelfSupport + 4) CHECK(pc[0][i] == pc[1][i]);
        for (int me = 0; me < 2; me++) {
            uint8_t team[4], com[4];
            for (int rc = 0; rc < 4; rc++) { team[rc] = pc[me][jspk::kOffRecords + jspk::kRecSize * rc + jspk::kRecTeam]; com[rc] = pc[me][jspk::kOffRecords + jspk::kRecSize * rc + jspk::kRecCom] != 0; }
            CHECK(jnf::placement_net_slot(team, com, 0) == 0);   // the host's fighter takes net slot 0 = its rollback slot
            CHECK(jnf::placement_net_slot(team, com, 1) == 1);   // the joiner's fighter takes net slot 1
            const bool together = jspk::agreed_mode(p.pick) == jspk::TOGETHER;
            CHECK(together ? team[0] == team[1] : team[0] != team[1]);
            const uint16_t f0 = jspk::be16(pc[me] + jspk::kOffRecords + jspk::kRecFighter), f1 = jspk::be16(pc[me] + jspk::kOffRecords + jspk::kRecSize + jspk::kRecFighter);
            if (p.pick[0].fighter != jspk::kRandom) CHECK(f0 == p.pick[0].fighter);   // each player's own pick on its own record
            if (p.pick[1].fighter != jspk::kRandom) CHECK(f1 == p.pick[1].fighter);
        }
        runs++;
    }
    CHECK(runs == 2000);
}

// ---- 6. FIX (pick7): ownership after Select character with the fighter map's planned-seat contract ----
// The session start after a Select character runs jsp_native_fight_map with the plan's seats ((1 << total_players) - 1, jspo::plan_seats), like
// every session start. Here: two PCs go through the real flow (switch handshake, prompt, Select character, picks, READY), each writes the agreed
// pair into its copy of the room (team bytes a PC's versus_apply may have swapped), and the map the session start builds from it
// (jspo::predict_map = the builder's placement + jnf::net_ports with the seats) must be ok, the same on both PCs, and give each PC's pad its own
// fighter. The plan's seat check at the menu's open, and the refusals the session start keeps, are checked too.
static void put_rec(uint8_t* img, int rc, uint16_t member, bool com, uint8_t team, uint16_t fighter)
{
    uint8_t* p = img + jspk::kOffRecords + jspk::kRecSize * rc;
    jspk::put_be16(p + jspk::kRecMember, member); p[jspk::kRecCom] = com ? 1 : 0; p[jspk::kRecTeam] = team; jspk::put_be16(p + jspk::kRecFighter, fighter);
}
static void test_ownership_mapper()
{
    const uint8_t s01[2] = { 0, 1 }, s10[2] = { 1, 0 }, s02[2] = { 0, 2 }, s11[2] = { 1, 1 };
    CHECK(jspo::plan_seats(2) == 3 && jspo::plan_seats(4) == 15 && jspo::plan_seats(0) == 0 && jspo::plan_seats(5) == 0);
    CHECK(jspo::seats_ok(s01, 2, 2) && jspo::seats_ok(s10, 2, 2));
    CHECK(!jspo::seats_ok(s02, 2, 2) && !jspo::seats_ok(s11, 2, 2) && !jspo::seats_ok(s01, 2, 3) && !jspo::seats_ok(s01, 1, 1));
    std::mt19937 r(20261122u + 7u);
    int rooms = 0, together = 0, swapped = 0;
    for (int it = 0; it < 500; it++) {
        jspf::Flow h, j;
        CHECK(h.open(5000u + (uint64_t)it, 0, 2, s01) && j.open(5000u + (uint64_t)it, 1, 2, s01));
        uint64_t t = 0;
        auto step = [&] {
            h.pump(t, [&](const uint8_t* d, size_t n) { j.route(0, d, n, t); });
            j.pump(t, [&](const uint8_t* d, size_t n) { h.route(1, d, n, t); });
            h.poll(t); j.poll(t); t += 100;
        };
        for (int k = 0; k < 30; k++) step();
        CHECK(h.match_end(1, t) && j.match_end(1, t));
        CHECK(h.choose(jspg::CHARSELECT, t) && j.choose((r() & 1) ? jspg::REMATCH : jspg::CHARSELECT, t));   // mixed: Select character wins
        for (int k = 0; k < 10; k++) step();
        CHECK(h.picking() && j.picking());
        jspk::Pick ph, pj;
        ph.fighter = r() % 3 == 0 ? jspk::kRandom : (uint16_t)(r() % 42); pj.fighter = r() % 3 == 0 ? jspk::kRandom : (uint16_t)(r() % 42);
        ph.mode = (r() & 1) ? jspk::TOGETHER : jspk::VERSUS; pj.mode = (r() & 1) ? jspk::TOGETHER : jspk::VERSUS;   // the host's mode wins
        CHECK(h.submit(ph, true) && j.submit(pj, true));
        for (int k = 0; k < 30 && !(h.round().state() == jspk::READY && j.round().state() == jspk::READY); k++) step();
        jspf::PairOut pa[2];
        CHECK(h.pair(&pa[0]) && j.pair(&pa[1]));
        CHECK(jspo::slot_seats(pa[0].slot, pa[0].n) == jspo::plan_seats(2) && pa[0].self_slot == 0 && pa[1].self_slot == 1);
        // the room after the host's broadcast: host record 0 / member 1, joiner record 1 / member 2, two COMs; per PC team bytes
        uint8_t img[jspk::kRoomSize];
        for (uint32_t i = 0; i < jspk::kRoomSize; i++) img[i] = (uint8_t)r();
        for (int i = 0; i < 4; i++) img[jspk::kOffSlotTeam + i] = (uint8_t)(r() % 3);
        put_rec(img, 0, 1, false, (uint8_t)(r() % 2), (uint16_t)(r() % 42)); put_rec(img, 1, 2, false, (uint8_t)(r() % 2), (uint16_t)(r() % 42));
        put_rec(img, 2, 0, true, (uint8_t)(r() % 2), 0x20); put_rec(img, 3, 0, true, (uint8_t)(r() % 2), 0x21);
        uint8_t pc[2][jspk::kRoomSize];
        std::memcpy(pc[0], img, sizeof img); std::memcpy(pc[1], img, sizeof img);
        if (r() & 1) { std::swap(pc[1][jspk::kOffRecords + jspk::kRecSize + jspk::kRecTeam], pc[1][jspk::kOffRecords + jspk::kRecSize * 2 + jspk::kRecTeam]); swapped++; }
        for (int me = 0; me < 2; me++) CHECK(jspf::apply_pair(pc[me], pa[me]) == jspk::Result::Ok);
        const jspo::Map m0 = jspo::predict_map(pc[0], jspo::plan_seats(2)), m1 = jspo::predict_map(pc[1], jspo::plan_seats(2));
        CHECK(m0.err == jnf::MAP_OK && m1.err == jnf::MAP_OK);
        CHECK(jspo::own_ok(m0, 0) && jspo::own_ok(m1, 1));                                        // each PC's pad -> its own fighter
        CHECK(!std::memcmp(m0.port, m1.port, sizeof m0.port) && !std::memcmp(m0.net_of_rec, m1.net_of_rec, sizeof m0.net_of_rec));   // one map
        for (int rc = 0; rc < 2; rc++) CHECK(m0.net_of_rec[rc] >= 0 && m0.port[m0.net_of_rec[rc]] == rc);
        CHECK(m0.net_of_rec[2] < 0 && m0.net_of_rec[3] < 0);                                     // the COMs get no net slot
        const bool tog = jspk::agreed_mode(pa[0].pick) == jspk::TOGETHER;
        const uint8_t t0 = pc[0][jspk::kOffRecords + jspk::kRecTeam], t1 = pc[0][jspk::kOffRecords + jspk::kRecSize + jspk::kRecTeam];
        CHECK(tog ? t0 == t1 : t0 != t1);
        together += tog;
        if (ph.fighter != jspk::kRandom) CHECK(jspk::be16(pc[0] + jspk::kOffRecords + jspk::kRecFighter) == ph.fighter);
        if (pj.fighter != jspk::kRandom) CHECK(jspk::be16(pc[1] + jspk::kOffRecords + jspk::kRecSize + jspk::kRecFighter) == pj.fighter);
        // Fight again after it: the same pair is written again and maps the same way
        h.fight_running(); j.fight_running();
        CHECK(h.match_end(2, t) && j.match_end(2, t) && h.choose(jspg::REMATCH, t) && j.choose(jspg::REMATCH, t));
        for (int k = 0; k < 30 && !(h.round().state() == jspk::READY && j.round().state() == jspk::READY); k++) step();
        jspf::PairOut again; CHECK(h.pair(&again) && again.pick[0] == pa[0].pick[0] && again.pick[1] == pa[0].pick[1]);
        uint8_t re[jspk::kRoomSize]; std::memcpy(re, img, sizeof img);
        CHECK(jspf::apply_pair(re, again) == jspk::Result::Ok && !std::memcmp(re + jspk::kOffRecords, pc[0] + jspk::kOffRecords, 4 * jspk::kRecSize));
        rooms++;
    }
    CHECK(rooms == 500 && together > 50 && swapped > 50);
    // what the session start refuses stays refused: a stale third member (the roster write refuses it, and the native roster maps MAP_EXTRA) ...
    uint8_t img[jspk::kRoomSize] = {};
    put_rec(img, 0, 1, false, 0, 3); put_rec(img, 1, 2, false, 1, 4); put_rec(img, 2, 3, false, 0, 5); put_rec(img, 3, 0, true, 1, 6);
    jspf::PairOut p; p.n = 2; p.slot[0] = 0; p.slot[1] = 1;
    uint8_t keep[jspk::kRoomSize]; std::memcpy(keep, img, sizeof img);
    CHECK(jspf::apply_pair(img, p) == jspk::Result::HumansMismatch && !std::memcmp(keep, img, sizeof img));
    CHECK(jspo::predict_map(img, jspo::plan_seats(2)).err == jnf::MAP_EXTRA && !jspo::own_ok(jspo::predict_map(img, 3), 0));
    // ... and a joiner on record 2 (not a planned seat): the menu's link would not open (seats_ok) and the map refuses
    put_rec(img, 1, 0, true, 1, 4); put_rec(img, 2, 2, false, 1, 5);
    const uint8_t s02b[2] = { 0, 2 };
    CHECK(!jspo::seats_ok(s02b, 2, 2) && jspo::predict_map(img, jspo::plan_seats(2)).err != jnf::MAP_OK);
}

int main()
{
    test_ownership();
    test_ownership_mapper();
    test_pad_gate();
    test_box_basic();
    test_box_gates();
    test_join_ready();
    test_map1();
    if (fails) { std::printf("net pick drive: %d check(s) FAILED\n", fails); return 1; }
    std::printf("net pick drive: pad gate (capture, late-pick start block, injected presses), notice-box Cross (timing, cap, gates, loading), joiner's Ready, map1 timelines, ownership after the roster write (2000 rooms), ownership after Select character through the flow and the fighter map's planned seats (500 rooms + refusals) ok\n");
    return 0;
}
