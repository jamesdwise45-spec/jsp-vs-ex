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
inline PadOut pad_gate(uint16_t raw, uint16_t* prev, bool capture, bool block, uint16_t press, bool press_live)
{
    PadOut o;
    o.edges = (uint16_t)(raw & ~*prev);
    *prev = raw;
    o.buttons = raw;
    if (capture) { o.buttons = 0; o.centre = true; }
    else if (block) o.buttons = (uint16_t)(raw & ~kBtnCross);
    else if (press_live) o.buttons = (uint16_t)(raw | press);
    return o;
}

// The notice-box Cross. One per post-game drive; step() once per main-thread frame.
struct BoxIn {
    bool reached = false;          // the drive reached the room (in the room, starting, or by hand)
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
