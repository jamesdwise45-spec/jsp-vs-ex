// jsp_net_pick_drive.h -- ENHANCEMENT (experimental rollback netcode; post-game / pick, JSP_ROLLBACK_PICK=1, default 0): the pure rules of the
// after-match drive in jsp_net_game.cpp that press buttons or hold them back for the player. jsp_net_game.cpp keeps the state, the clocks and the
// guest calls; these functions only decide. No Windows, no game memory: installer/tools/net_pick_drive_test.cpp runs them on the host.
//   pad_gate     the prompt's pad filter on port 0 (patch 6501): capture (the prompt / picker holds the pad), the host's start block (late-pick
//                block: no "Go to battle" Cross while a pick round runs and the start is not safe yet), and the injected presses.
//   BoxCross     FIX (mapping run U4, map1): after a match the game opens notice boxes over the room ("You got a new title.", one OK button) a moment
//                AFTER the room page is up; while one is open the room page's update targets stop and the game waits for Cross. Once the room was
//                reached, a room page hidden for 2.5 s with no fight drawn gets one Cross a second (at most 8 in a row; again after the room shows).
//                Never while the prompt / picker holds the pad or the host's start block is on (FIX, host test: the block swallowed all 8 Crosses of a
//                long pick round and the box stayed until the 60 s start timeout), never on the player's own Battle settings page, never once the online loading scene
//                ran after the room was reached (the start worked; a Cross there could skip a screen on one PC only).
//   live_press / autopilot_pad   FIX (pick8 in-game run s1a, test path only): the AUTO_ONLINE autopilot's pad outside a match keeps the drive's press.
//   join_ready   TODO-MAPPING (U7): the joiner's Ready Cross in the room (JSP_PICK_JOIN_READY: 0 never, 1 once per round, 2 once and then every 5 s).
#pragma once
#include <cstdint>

namespace jspd {

constexpr uint16_t kBtnCross = 0x4000, kBtnCircle = 0x2000, kBtnSquare = 0x8000, kBtnUp = 0x0010, kBtnDown = 0x0040, kBtnLeft = 0x0080, kBtnRight = 0x0020;

// The pad filter's decision for one read of port 0 outside a running match. raw: the player's buttons; *prev: the previous raw buttons (edge
// detection, updated here). capture: the game reads a released pad (buttons 0, sticks centred) and the new presses go to `edges` for the prompt.
// block: Cross is taken out of the player's buttons, and an injected press is NOT added (the host's own start must wait: Flow::block_start).
// press / press_live: the drive's injected buttons, OR-ed in while live (only when neither capture nor block).
struct PadOut { uint16_t buttons = 0; uint16_t edges = 0; bool centre = false; };
// FIX (test7 screenshot run): the menus read only the D-pad; a player on the left stick could not move the cursor. stick_dirs: the left stick
// as D-pad bits (beyond about half of its travel), used for the menus' edges only; the game's own buttons never get them.
inline uint16_t stick_dirs(uint16_t lx, uint16_t ly)
{
    uint16_t d = 0;
    if (ly < 64) d |= kBtnUp; else if (ly > 192) d |= kBtnDown;
    if (lx < 64) d |= kBtnLeft; else if (lx > 192) d |= kBtnRight;
    return d;
}
// FIX (test8 mapping run, JSP_PICK_NATIVE_SELECT=1): the block holds back the host's start, which is a Cross. The page step's Square for the
// Battle settings page is injected while the block is on (a pick round runs, so the host's start is not safe yet); the block dropped it and the
// host's page never opened (two Squares, then the port picker). Under the block an injected press without Cross still goes through.
// FIX (test8 review M2): on the game's own Battle settings page (JSP_PICK_NATIVE_SELECT=1) Cross is Confirm, and the start block (on while
// this PC's page step runs, since the host's start waits for it) stripped the player's own Cross there: a human host could not pick. The
// block is for the room page, where the host's Cross is "Go to battle": while this PC's page ran in the last kPageOpenMs (about 3 frames)
// the player's own Cross passes; in the room (waiting, back, not saved) the block holds as before.
constexpr uint64_t kPageOpenMs = 100;
inline bool page_open(uint64_t page_seen_ms, uint64_t now) { return page_seen_ms && now >= page_seen_ms && now - page_seen_ms < kPageOpenMs; }
inline bool own_block(bool block, uint64_t page_seen_ms, uint64_t now) { return block && !page_open(page_seen_ms, now); }
inline uint16_t blocked_press(uint16_t press, bool press_live) { return press_live && !(press & kBtnCross) ? press : 0; }
inline PadOut pad_gate(uint16_t raw, uint16_t* prev, bool capture, bool block, uint16_t press, bool press_live, uint16_t dirs = 0)
{
    PadOut o;
    const uint16_t seen = (uint16_t)(raw | dirs);
    o.edges = (uint16_t)(seen & ~*prev);
    *prev = seen;
    o.buttons = raw;
    if (capture) { o.buttons = 0; o.centre = true; }
    else if (block) o.buttons = (uint16_t)((raw | blocked_press(press, press_live)) & ~kBtnCross);
    else if (press_live) o.buttons = (uint16_t)(raw | press);
    return o;
}

// FIX (pick8 in-game run s1a, test path only): the AUTO_ONLINE autopilot (jsp_net_native.cpp, JSP_NATIVE_TRACE / AUTO_ONLINE) installs its pad filter
// before pick_pad, so pick_pad runs first and then calls it; the autopilot wrote 0 to the pad on every step outside a running match (only its own
// scripted press passed), which erased every press the after-match drive had just OR-ed in (the to-room Cross, the host's start Cross, the
// notice-box Cross, the joiner's Ready). In run s1a the host's start never reached the game and the drive went "by hand" after 60 s. On a player's
// PC there is no autopilot; nothing changes there.
// live_press: the drive's press that pad_gate adds to a released pad right now (none while capture or not live; under block, none with Cross).
// autopilot_pad: the autopilot's buttons outside a match: its own scripted press while live, else the drive's live press, else nothing.
inline uint16_t live_press(bool capture, bool block, uint16_t press, bool press_live) { return capture || !press_live ? 0 : block ? blocked_press(press, true) : press; }
inline uint16_t autopilot_pad(bool scripted_live, uint16_t scripted, uint16_t pick_live) { return scripted_live ? scripted : pick_live; }

// The notice-box Cross. One per post-game drive; step() once per main-thread frame.
// box_reached: BoxIn::reached from the drive. In the room or starting: yes. By hand: only when the room page was seen in this round (the start
// timed out); after the 45 s to-room timeout with no room page ever seen, no notice-box Crosses go to whatever screen is up.
inline bool box_reached(bool in_room_or_starting, bool by_hand, bool room_seen) { return in_room_or_starting || (by_hand && room_seen); }
struct BoxIn {
    bool reached = false;          // the drive reached the room (in the room, starting, or by hand after the room was seen): box_reached
    bool room_now = false;         // a room page update target ran in the last 500 ms and no fight HUD for 1.5 s
    bool fight_now = false;        // a fight HUD is drawn
    bool capture = false;          // the prompt / picker holds the pad
    bool own_page = false;         // the player's own Square (or Circle on a free pad) in the last 30 s, or the host's native Battle settings page steps
    bool blocked = false;          // the host's start block is on (pad_gate drops every injected press): a Cross now would never reach the game
    uint64_t room_since = 0;       // when the room page was first seen in this drive (0 = not yet)
    uint64_t loading_seen = 0;     // when the online loading scene last ran (0 = never)
};
struct BoxCross {
    static constexpr uint64_t kHiddenMs = 2500, kGapMs = 1000;
    static constexpr int kMax = 8;
    enum Out { NONE, PRESS, BACK };
    uint64_t away_since = 0;   // the room page hidden since (0 = shown, or not reached)
    int away_n = 0;            // Crosses sent since the room page went away
    bool loading = false;      // the online loading scene ran after the room was reached: no more Crosses this round
    // press_t: the drive's last press of any kind. PRESS: press Cross now (the caller sets its press_t). BACK: the room page came back after
    // *back_n (> 0) Crosses (for the log line).
    Out step(const BoxIn& in, uint64_t now, uint64_t press_t, int* back_n)
    {
        if (!in.reached) loading = false;
        else if (in.loading_seen && in.room_since && in.loading_seen > in.room_since) loading = true;
        if (!in.reached || in.room_now || in.fight_now || loading) {
            const int n = away_n;
            away_since = 0; away_n = 0;
            if (n) { if (back_n) *back_n = n; return BACK; }
            return NONE;
        }
        if (!away_since) { away_since = now; return NONE; }
        if (!in.capture && !in.own_page && !in.blocked && away_n < kMax && now - away_since >= kHiddenMs && now - press_t >= kGapMs) { away_n++; return PRESS; }
        return NONE;
    }
};

// The joiner's Ready Cross (TODO-MAPPING U7). mode: JSP_PICK_JOIN_READY. in_room: the drive is "in the room" (not starting / by hand). picking:
// this PC's pick round still runs (Select character, not locked by every player). *joined: the once-per-round press was made (reset by the caller
// when a new drive to the room starts). press_t: the drive's last press. true = press Cross now.
inline bool join_ready(int mode, bool in_room, bool host, bool picking, bool room_now, bool* joined, uint64_t now, uint64_t press_t)
{
    if (mode < 1 || !in_room || host || picking || !room_now) return false;
    if (!*joined) { *joined = true; return true; }
    return mode == 2 && now - press_t >= 5000;
}

}  // namespace jspd
