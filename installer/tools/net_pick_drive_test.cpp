// Host test of the after-match drive rules (installer/port/jsp/jsp_net_pick_drive.h): the prompt's pad gate (capture, the host's late-pick start
// block, injected presses), the notice-box Cross of the mapping run's U4 fix (map1: "You got a new title." over the room), and the joiner's Ready
// Cross (U7). It also replays the two map1 timelines on a model room page, with the host's start block on, to show the box always closes and the
// start never runs before every pick is committed. No game, no GPU.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>
#include "../port/jsp/jsp_net_pick_drive.h"
#include "../port/jsp/jsp_net_pick_flow.h"      // apply_pair: the roster write at the builder (ownership after Select character)
// glibc's <math.h> declares a function jnf (Bessel); the port's namespace of the same name is renamed for this test only (the math headers are in above)
#define jnf jsp_native_fight_ns
#include "../port/jsp/jsp_net_native_fight.h"   // jnf::placement_net_slot: the builder's net slot of a record (swapped-controls fix)
#include "../port/jsp/jsp_net_pick_own.h"       // FIX (pick7): the planned seats and the fighter map the session start builds (jnf::net_ports)
#include "../port/jsp/jsp_net_menu4.h"         // ENHANCEMENT (test13): the 3-4 player menu glue and its leave rule

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
    // the left stick moves the menus' cursor (edges only), never the game's buttons
    CHECK(stick_dirs(128, 128) == 0 && stick_dirs(128, 0) == kBtnUp && stick_dirs(128, 255) == kBtnDown && stick_dirs(0, 128) == kBtnLeft && stick_dirs(255, 128) == kBtnRight);
    CHECK(stick_dirs(100, 160) == 0);   // small deflections do nothing
    prev = 0;
    o = pad_gate(0, &prev, true, false, 0, false, stick_dirs(128, 0));
    CHECK(o.buttons == 0 && o.centre && o.edges == kBtnUp);
    o = pad_gate(0, &prev, true, false, 0, false, stick_dirs(128, 10));
    CHECK(o.edges == 0);   // held up: once
    o = pad_gate(0, &prev, true, false, 0, false, stick_dirs(128, 128));
    o = pad_gate(0, &prev, true, false, 0, false, stick_dirs(128, 250));
    CHECK(o.edges == kBtnDown);
    o = pad_gate(0, &prev, false, false, 0, false, stick_dirs(128, 0));
    CHECK(o.buttons == 0 && !o.centre);   // free pad: the stick never becomes a D-pad press for the game
    prev = 0;
    // capture wins over block
    o = pad_gate(kBtnCircle, &prev, true, true, 0, false);
    CHECK(o.buttons == 0 && o.centre && o.edges == kBtnCircle);
    // late-pick block (the host's start): the player's own Cross is swallowed, other buttons pass, and NO injected Cross goes through
    prev = 0;
    o = pad_gate(kBtnCross | kBtnSquare, &prev, false, true, kBtnCross, true);
    CHECK(o.buttons == kBtnSquare && !o.centre && o.edges == (kBtnCross | kBtnSquare));
    o = pad_gate(0, &prev, false, true, kBtnCross, true);
    CHECK(o.buttons == 0);
    o = pad_gate(kBtnCircle, &prev, false, true, kBtnCross, true);
    CHECK(o.buttons == kBtnCircle);   // Leave stays possible in the room (Circle) while the start is blocked
    // test8 mapping run: the page step's injected Square goes through the block (the host's Battle settings page must open); a Cross in it does not
    o = pad_gate(0, &prev, false, true, kBtnSquare, true);
    CHECK(o.buttons == kBtnSquare);
    o = pad_gate(0, &prev, false, true, kBtnSquare, false);
    CHECK(o.buttons == 0);
    o = pad_gate(0, &prev, false, true, (uint16_t)(kBtnSquare | kBtnCross), true);
    CHECK(o.buttons == 0);
    o = pad_gate(kBtnCross, &prev, false, true, kBtnSquare, true);
    CHECK(o.buttons == kBtnSquare);
    // review M2: the player's own Cross on the game's own Battle settings page (Confirm there) passes the block while the page ran in the
    // last kPageOpenMs; in the room (the page stale, or never seen) the block strips it as before
    CHECK(!own_block(true, 1000, 1050) && !own_block(true, 1000, 1099) && own_block(true, 1000, 1100) && own_block(true, 0, 1050));
    CHECK(!own_block(false, 0, 1050) && !own_block(false, 1000, 5000));
    CHECK(own_block(true, 2000, 1000));   // a page time ahead of now (a clock read out of order): blocked
    o = pad_gate(kBtnCross, &prev, false, own_block(true, 5000, 5033), 0, false);
    CHECK(o.buttons == kBtnCross);         // the host on its page: Cross confirms there
    o = pad_gate(kBtnCross, &prev, false, own_block(true, 5000, 5400), 0, false);
    CHECK(o.buttons == 0);                 // the host back in the room, the start not safe yet: its Cross is still swallowed
    o = pad_gate(kBtnCross, &prev, false, own_block(false, 0, 5400), 0, false);
    CHECK(o.buttons == kBtnCross);         // the joiner (never blocked): its Cross passes on the page and in the room
    // block lifted: the injected Cross reaches the game again
    o = pad_gate(0, &prev, false, false, kBtnCross, true);
    CHECK(o.buttons == kBtnCross);
}

// FIX (pick8 in-game run s1a): the AUTO_ONLINE autopilot's pad filter runs after pick_pad (pick_pad calls it) and used to write 0 outside a
// match. live_press must be exactly what pad_gate added to a released pad, and the autopilot must keep it (its own scripted press wins). Then the
// s1a hand-off: the host's start Cross every 2.5 s for 60 s through the chain pick_pad -> autopilot reaches the game; with the old autopilot none did.
static void test_autopilot_pad()
{
    const uint16_t presses[] = { 0, kBtnCross, kBtnSquare, kBtnCircle, (uint16_t)(kBtnCross | kBtnUp) };
    int n = 0;
    for (int c = 0; c < 2; c++) for (int bl = 0; bl < 2; bl++) for (int live = 0; live < 2; live++) for (uint16_t pr : presses) {
        uint16_t prev = 0;
        const PadOut o = pad_gate(0, &prev, c, bl, pr, live);
        const uint16_t lp = live_press(c, bl, pr, live);
        CHECK(lp == o.buttons);                              // what pick_pad added to a released pad, nothing else
        CHECK(autopilot_pad(false, 0, lp) == o.buttons);     // the autopilot between its own presses keeps it
        CHECK(autopilot_pad(true, kBtnDown, lp) == kBtnDown); // its own scripted press wins while live
        n++;
    }
    CHECK(n == 40);
    // the s1a timeline: room page up, the host's start presses (120 ms live, one every 2500 ms), the game reads each 33 ms frame
    int reach_new = 0, reach_old = 0; bool was_new = false, was_old = false; uint64_t press_until = 0;
    for (uint64_t t = 0; t < 60000; t += 33) {
        if (t % 2500 < 33) press_until = t + 150;
        uint16_t prev = 0;
        const bool live = t < press_until;
        const uint16_t after_pick = pad_gate(0, &prev, false, false, kBtnCross, live).buttons;   // pick_pad's decision (the player's pad is idle)
        const uint16_t game_new = autopilot_pad(false, 0, live_press(false, false, kBtnCross, live));
        const uint16_t game_old = 0;   // the old autopilot: *b = scripted ? button : 0
        CHECK(game_new == after_pick);
        const bool dn = (game_new & kBtnCross) != 0, dold = (game_old & kBtnCross) != 0;
        if (dn && !was_new) reach_new++;
        if (dold && !was_old) reach_old++;
        was_new = dn; was_old = dold;
    }
    CHECK(reach_new == 24);   // 60 s / 2.5 s: every start Cross reaches the game
    CHECK(reach_old == 0);    // the stuck hand-off of run s1a
    // the host's late-pick block still holds the start back through the autopilot
    CHECK(autopilot_pad(false, 0, live_press(false, true, kBtnCross, true)) == 0);
    // ... and the host's page Square reaches the game through the autopilot under the block (test8 mapping run: it never did)
    CHECK(autopilot_pad(false, 0, live_press(false, true, kBtnSquare, true)) == kBtnSquare);
    // live_press is what pad_gate adds to a released pad, under every gate
    for (int cap = 0; cap < 2; cap++) for (int blk = 0; blk < 2; blk++) for (int lv = 0; lv < 2; lv++)
        for (uint16_t pr : { (uint16_t)0, kBtnCross, kBtnSquare, (uint16_t)(kBtnCross | kBtnSquare) }) {
            uint16_t pv = 0;
            CHECK(pad_gate(0, &pv, cap, blk, pr, lv).buttons == live_press(cap, blk, pr, lv));
        }
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
// review N1: by hand after the to-room timeout with no room page ever seen: no Cross; by hand after the start timed out (room seen): Crosses
static void test_box_by_hand()
{
    CHECK(jspd::box_reached(true, false, false) && jspd::box_reached(true, false, true));
    CHECK(!jspd::box_reached(false, true, false) && jspd::box_reached(false, true, true));
    CHECK(!jspd::box_reached(false, false, true));
    int back = 0;
    {
        BoxCross b; BoxIn in = reached_hidden(0); in.reached = jspd::box_reached(false, true, false);
        for (uint64_t t = 1000; t < 60000; t += 50) CHECK(b.step(in, t, 0, &back) == BoxCross::NONE);
        CHECK(b.away_n == 0);
    }
    {
        BoxCross b; BoxIn in = reached_hidden(0); in.reached = jspd::box_reached(false, true, true);
        int n = 0; uint64_t press_t = 0;
        for (uint64_t t = 1000; t < 60000; t += 50) if (b.step(in, t, press_t, &back) == BoxCross::PRESS) { press_t = t; n++; }
        CHECK(n == BoxCross::kMax);
    }
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

// ---- 7. ENHANCEMENT (test13, JSP_ROLLBACK_PICK_MULTI=1): 3-4 players ----
// (a) Ownership after Select character with the menu built from each PC's own plan (jspm4::menu_seats), peer ids != battle slots: every PC
// writes its committed pair into its copy of the room and the fighter map the session start builds (planned seats (1 << n) - 1) must give each
// PC's pad its own fighter, the same map on every PC. The TEAM bytes after the write are lane C's rule (jspf::apply_pair / jspk::roster_apply);
// this test only checks that whatever the teams are, each pad drives its own fighter and the PCs agree.
struct MRemote { uint8_t peer = 0, first_slot = 0, count = 1; };
struct MPlan { bool host = false; uint8_t total_players = 0, local_peer = 0, local_first = 0, local_count = 1; std::vector<MRemote> remotes; };
static void test_multi_ownership()
{
    std::mt19937 r(20261128u);
    int rooms = 0;
    for (int it = 0; it < 300; it++) {
        const int n = 3 + (it & 1);
        uint8_t slot_of[4] = { 0, 1, 2, 3 };
        std::shuffle(slot_of, slot_of + n, r);   // peer i plays battle slot slot_of[i]
        jspf::Flow f[4]; jspm4::Seats s[4];
        for (int i = 0; i < n; i++) {
            MPlan p; p.host = i == 0; p.total_players = (uint8_t)n; p.local_peer = (uint8_t)i; p.local_first = slot_of[i];
            for (int j = 0; j < n; j++) if (j != i) { MRemote m; m.peer = (uint8_t)j; m.first_slot = slot_of[j]; p.remotes.push_back(m); }
            s[i] = jspm4::menu_seats(p, true);
            CHECK(s[i].ok && f[i].open(7000u + (uint64_t)it, s[i].me, s[i].n, s[i].slots));
        }
        uint64_t t = 0;
        auto step = [&] {
            for (int i = 0; i < n; i++) f[i].pump(t, [&](const uint8_t* d, size_t len) { for (int k = 0; k < s[i].nto; k++) f[s[i].to[k]].route(i, d, len, t); });
            for (int i = 0; i < n; i++) f[i].poll(t);
            t += 100;
        };
        for (int k = 0; k < 30; k++) step();
        for (int i = 0; i < n; i++) CHECK(f[i].match_end(1, t));
        for (int i = 0; i < n; i++) CHECK(f[i].choose(i == (it % n) ? jspg::CHARSELECT : ((r() & 1) ? jspg::REMATCH : jspg::CHARSELECT), t));
        for (int k = 0; k < 10; k++) step();
        jspk::Pick pk[4];
        for (int i = 0; i < n; i++) { CHECK(f[i].picking()); pk[i].fighter = r() % 3 == 0 ? jspk::kRandom : (uint16_t)(r() % 42); pk[i].mode = (r() & 1) ? jspk::TOGETHER : jspk::VERSUS; CHECK(f[i].submit(pk[i], true)); }
        for (int k = 0; k < 40; k++) step();
        jspf::PairOut pa[4];
        for (int i = 0; i < n; i++) { CHECK(f[i].round().state() == jspk::READY && f[i].pair(&pa[i])); CHECK(pa[i].self_slot == slot_of[i] && jspo::slot_seats(pa[i].slot, n) == jspo::plan_seats(n)); }
        // the room after the host's broadcast: peer i's human record at slot_of[i] (member id 10 + i), COMs on the other records; random team bytes
        uint8_t img[jspk::kRoomSize];
        for (uint32_t i = 0; i < jspk::kRoomSize; i++) img[i] = (uint8_t)r();
        for (int i = 0; i < 4; i++) img[jspk::kOffSlotTeam + i] = (uint8_t)(r() % 3);
        for (int rc = 0; rc < 4; rc++) put_rec(img, rc, 0, true, (uint8_t)(r() % 2), (uint16_t)(0x20 + rc));
        for (int i = 0; i < n; i++) put_rec(img, slot_of[i], (uint16_t)(10 + i), false, (uint8_t)(r() % 2), (uint16_t)(r() % 42));
        uint8_t pc[4][jspk::kRoomSize];
        for (int i = 0; i < n; i++) { std::memcpy(pc[i], img, sizeof img); CHECK(jspf::apply_pair(pc[i], pa[i]) == jspk::Result::Ok); }
        jspo::Map m[4];
        for (int i = 0; i < n; i++) {
            m[i] = jspo::predict_map(pc[i], jspo::plan_seats(n));
            CHECK(m[i].err == jnf::MAP_OK && jspo::own_ok(m[i], slot_of[i]));   // this PC's pad -> its own fighter
            if (i) CHECK(!std::memcmp(m[i].port, m[0].port, sizeof m[0].port) && !std::memcmp(pc[i] + jspk::kOffRecords, pc[0] + jspk::kOffRecords, 4 * jspk::kRecSize));
            if (pk[i].fighter != jspk::kRandom) CHECK(jspk::be16(pc[i] + jspk::kOffRecords + jspk::kRecSize * slot_of[i] + jspk::kRecFighter) == pk[i].fighter);
        }
        rooms++;
    }
    CHECK(rooms == 300);
}

// (b) The leave rule's gate in native_poll (jspm4::leave_gate): hold while the room still has the link's player count, release when it drops or
// the wait is over (the caller then counts the room's tries afresh), forget it in another room.
static void test_leave_gate()
{
    using jspf::Act;
    CHECK(jspm4::leave_wait_applies(3, Act::LeaveClose, jspg::R_PEER_LEFT) && jspm4::leave_wait_applies(4, Act::PeerFroze, jspg::R_NONE));
    CHECK(!jspm4::leave_wait_applies(2, Act::LeaveClose, jspg::R_PEER_LEFT) && !jspm4::leave_wait_applies(2, Act::PeerFroze, jspg::R_NONE));   // 2 players: as before
    CHECK(!jspm4::leave_wait_applies(4, Act::LeaveClose, jspg::R_LINK_LOST) && !jspm4::leave_wait_applies(4, Act::Lost, jspg::R_NONE));
    CHECK(!jspm4::leave_wait_applies(4, Act::LeaveGiveUp, jspg::R_SELF) && !jspm4::leave_wait_applies(4, Act::Froze, jspg::R_NONE));
    jspm4::LeaveWait w;
    CHECK(jspm4::leave_gate(w, 55, 4, 0) == jspm4::Gate::None);   // nothing to wait for
    w = jspm4::leave_wait(55, 4, 1000);
    CHECK(jspm4::leave_gate(w, 55, 4, 1000) == jspm4::Gate::Hold && jspm4::leave_gate(w, 55, 4, 1000 + jspm4::kLeaveWaitMs - 1) == jspm4::Gate::Hold);
    CHECK(jspm4::leave_gate(w, 55, 3, 5000) == jspm4::Gate::Release && w.room == 0);   // 4 -> 3: the leaver is out, plan for 3
    CHECK(jspm4::leave_gate(w, 55, 3, 5100) == jspm4::Gate::None);                     // once
    w = jspm4::leave_wait(55, 3, 0);
    CHECK(jspm4::leave_gate(w, 55, 2, 10) == jspm4::Gate::Release);                    // 3 -> 2: the 2-player path
    w = jspm4::leave_wait(55, 4, 0);
    CHECK(jspm4::leave_gate(w, 55, 4, jspm4::kLeaveWaitMs) == jspm4::Gate::Release);   // the leaver stayed: after the wait, the plan as before
    w = jspm4::leave_wait(55, 4, 0);
    CHECK(jspm4::leave_gate(w, 56, 4, 10) == jspm4::Gate::None && w.room == 0);        // another room: forgotten
    CHECK(jspm4::leave_gate(w, 55, 4, 20) == jspm4::Gate::None);
    CHECK(jspm4::kLeaveWaitMs == jspg::kIdleMs);   // as long as the menu's own idle leave
}

int main()
{
    test_ownership();
    test_ownership_mapper();
    test_multi_ownership();
    test_leave_gate();
    test_pad_gate();
    test_autopilot_pad();
    test_box_basic();
    test_box_gates();
    test_box_by_hand();
    test_join_ready();
    test_map1();
    if (fails) { std::printf("net pick drive: %d check(s) FAILED\n", fails); return 1; }
    std::printf("net pick drive: pad gate (capture, late-pick start block, injected presses), the AUTO_ONLINE autopilot keeps the drive's press, notice-box Cross (timing, cap, gates, loading, by hand only after the room was seen), joiner's Ready, map1 timelines, ownership after the roster write (2000 rooms), ownership after Select character through the flow and the fighter map's planned seats (500 rooms + refusals), test13: 3-4 player ownership after Select character with the menu from each PC's own plan (300 rooms, peer != slot), the leave rule's gate ok\n");
    return 0;
}
