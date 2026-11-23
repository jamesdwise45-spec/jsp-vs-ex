// jsp_net_pick_page.h -- ENHANCEMENT (experimental rollback netcode; post-game / pick, JSP_ROLLBACK_PICK=1 AND JSP_PICK_NATIVE_SELECT=1, both default 0):
// Select character on the game's own character select for EVERY player. After "Select character" each PC, once back on the Friendly Match
// room page, opens the room's own Battle settings page (the screen the room owner already uses: Character Select with the Battle Character and
// Support Character portraits, then the Title List / Character List with the cursor) by an injected Square, and the player picks there. While
// the page is up this PC's pick is read from its own pick block (R+0x08 fighter, R+0x0C support) and sent unlocked, so the other player sees it
// and the round's idle auto-lock takes what the page shows; when the player is back on the room page the pick read there is locked (Ready).
// The roster both PCs build from is still the agreed pair of jsp_net_pick.h (PickAgreement + roster_apply at the builder): the page only
// replaces the port's list picker as the source of this PC's pick.
//
// Why both players can: the mapping run map1 (U3) showed the joiner's Square opens the same page objects as the owner's (update targets
// 0x002B8388 vt 0x008810D8, 0x002C2CEC vt 0x00881220, 0x002BCDB8 vt 0x00881140): the room has no owner gate on it, only the joiner's button bar
// hides it. Those three are vtable slot 4 of their classes, the same slot as the Free Battle select's update 0x0019AA74 (vt 0x0087C3D0) and the
// online loading scene's 0x002ECD30 (vt 0x00882220): the per-frame update, so "seen in the last 500 ms" tells that the page is up.
// TODO-MAPPING (P1): that they run every frame while the page is up and stop when it closes (JSP_PICK_TRACE=1's per-second [ptrace] ui lines).
//
// This header is the page step of one PC, pure (no Windows, no game memory): installer/tools/net_pick_page_test.cpp runs it. jsp_net_game.cpp
// feeds it the room-page and page signals once per frame and acts on what step() returns. States:
//   WAIT     on the room page; the Square goes out once the room page was seen for settle_ms without a break (a notice box over the room
//            stops the room page's update; the notice-box Cross of jsp_net_pick_drive.h closes it first)
//   SQUARE   Square sent; the page must show within open_wait_ms, else WAIT again (tries times), then FALLBACK (the port's list picker)
//   OPEN     the page is up: the player picks; read_live every frame
//   BACK     the page is gone and the room page is up again; after back_ms without the page: lock (DONE). A page shorter than min_open_ms opens
//            again once (a Square that landed on a closing page, or a player who backed out by reflex) before the pick is locked as read
//   DONE     the pick is locked; this PC's "in the room" flag may go out (the host's start waits for every PC's flag, so it never presses
//            "Go to battle" while another player is still on its page)
//   FALLBACK the page could not be opened or tracked: the port's list picker, as with JSP_PICK_NATIVE_SELECT=0
//   GIVEUP   still not back in the room deadline_ms after the round started (default: the round's idle lock + its 60 s wait, 180 s): this PC
//            leaves the round (the link closes, the room stays); the other PC hears the link close (its round ends as Connection lost)
#pragma once
#include <cstdint>

namespace jsps {

inline constexpr const char* kKey = "JSP_PICK_NATIVE_SELECT";
inline constexpr const char* kPageTgKey = "JSP_PICK_PAGE_TG";
// The Battle settings page's per-frame updates (U3, map1); JSP_PICK_PAGE_TG overrides them (hex, comma-separated, at most 4).
inline constexpr uint32_t kPageTg[4] = { 0x002B8388u, 0x002C2CECu, 0x002BCDB8u, 0 };
// JSP_PICK_NATIVE_SELECT: exactly "1" = the game's own page for every player; anything else = the port's list picker (today).
inline bool key_on(const char* v) { return v && v[0] == '1' && !v[1]; }

// JSP_PICK_PAGE_TG: up to 4 hex addresses (with or without 0x), separated by commas; a bad entry ends the list. Returns the count; 0 = keep
// the defaults (out is left as it was).
inline int parse_targets(const char* v, uint32_t out[4])
{
    if (!v) return 0;
    uint32_t t[4] = {}; int k = 0;
    const char* q = v;
    while (*q && k < 4) {
        if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2;
        uint32_t x = 0; int digits = 0;
        for (; *q && *q != ','; q++, digits++) {
            const char c = *q;
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0 || digits >= 8) return 0;
            x = x << 4 | (uint32_t)d;
        }
        if (!digits || !x) return 0;
        t[k++] = x;
        if (*q == ',') q++;
    }
    if (*q || !k) return 0;   // more than 4, or none
    for (int i = 0; i < 4; i++) out[i] = t[i];
    return k;
}

// The pick read back from this PC's own pick block. fighter_ok(id): a fighter the port can name (jnames::fighter_name); support_ok(id): a
// Support the port can name, when Support ids are on (JSP_PICK_SUPPORT_IDS=1; else every support reads as Random = the room's own support, which
// the native record upload carries: TODO-MAPPING U1b). 0x133 = Random. Anything else (a unit the port does not know) reads as Random, and
// *odd is set for the log.
inline constexpr uint16_t kRandom = 0x133;
template <class FOk, class SOk>
inline void read_pick(uint32_t raw_fighter, uint32_t raw_support, FOk&& fighter_ok, SOk&& support_ok, uint16_t* fighter, uint16_t* support, bool* odd)
{
    *odd = false;
    if (raw_fighter == kRandom || (raw_fighter <= 0xFFFFu && fighter_ok((int)raw_fighter))) *fighter = (uint16_t)raw_fighter;
    else { *fighter = kRandom; *odd = true; }
    if (raw_support == kRandom || (raw_support <= 0xFFFFu && support_ok((int)raw_support))) *support = (uint16_t)raw_support;
    else *support = kRandom;   // not odd: with Support ids off every Support reads as the room's own
}

enum class St : uint8_t { OFF, WAIT, SQUARE, OPEN, BACK, DONE, FALLBACK, GIVEUP };
inline const char* st_name(St s)
{
    return s == St::OFF ? "off" : s == St::WAIT ? "waiting for the room page" : s == St::SQUARE ? "Square sent" : s == St::OPEN ? "page open"
         : s == St::BACK ? "back in the room" : s == St::DONE ? "done" : s == St::FALLBACK ? "port picker" : "gave up";
}

struct Cfg {
    uint32_t settle_ms = 1500;      // the room page shown this long before the Square
    uint32_t open_wait_ms = 5000;   // the page must show this soon after the Square
    uint32_t back_ms = 1000;        // the room page back, the page gone, this long: the pick is locked
    uint32_t min_open_ms = 1500;    // a page shorter than this opens again (once)
    int tries = 2;                  // Squares before the port picker takes over (and page openings in all)
    uint64_t deadline_ms = 180000;  // not back in the room this long after start(): GIVEUP
    uint32_t warn_ms = 30000;       // the countdown shows in the last warn_ms before the deadline
};
struct In {
    uint64_t now = 0;
    bool room_now = false;   // the room page's update ran in the last 500 ms and no fight HUD for 1.5 s (jsp_net_game.cpp room_now)
    bool page_now = false;   // a page update target ran in the last 500 ms
    bool fight_now = false;  // a fight HUD is drawn (the next fight started: the step ends)
};
struct Out {
    bool square = false;     // press Square now
    bool read_live = false;  // read the pick block and send it unlocked (the page is up)
    bool lock = false;       // read the pick block and lock it (once)
    bool fallback = false;   // switch to the port's list picker (once)
    bool give_up = false;    // leave the round (once)
    bool opened = false, closed = false, retry = false;   // for the log (once each time)
};

class PageStep {
public:
    void start(uint64_t now, const Cfg& c) { cfg_ = c; st_ = St::WAIT; t0_ = t_ = now; room_seen_ = false; room_t_ = 0; open_t_ = 0; back_t_ = 0; squares_ = 0; opens_ = 0; }
    void stop() { st_ = St::OFF; }
    St state() const { return st_; }
    // The page step owns the pad's meaning: no notice-box Cross, no Ready Cross, no picker capture while it may be on the page.
    bool on_page() const { return st_ == St::SQUARE || st_ == St::OPEN || st_ == St::BACK; }
    bool active() const { return st_ == St::WAIT || on_page(); }
    // This PC may say "in the room" (the pick is locked as read, or the port picker took over).
    bool finished() const { return st_ == St::DONE || st_ == St::FALLBACK; }
    int squares() const { return squares_; }
    // Seconds to the deadline in the last warn_ms, else -1.
    int seconds_left(uint64_t now) const
    {
        if (!active()) return -1;
        const uint64_t el = now - t0_;
        if (el >= cfg_.deadline_ms) return 0;
        const uint64_t left = cfg_.deadline_ms - el;
        return left <= cfg_.warn_ms ? (int)((left + 999) / 1000) : -1;
    }

    Out step(const In& in)
    {
        Out o;
        if (!active()) return o;
        if (in.now - t0_ >= cfg_.deadline_ms) { st_ = St::GIVEUP; o.give_up = true; return o; }
        if (in.fight_now) return o;   // the ended fight's HUD (or a start by hand): no press over a fight
        switch (st_) {
        case St::WAIT:
            if (in.page_now) { st_ = St::OPEN; open_t_ = in.now; opens_++; o.opened = true; o.read_live = true; break; }   // the player's own Square
            if (!in.room_now) { room_seen_ = false; break; }
            if (!room_seen_) { room_seen_ = true; room_t_ = in.now; }
            if (in.now - room_t_ >= cfg_.settle_ms) { st_ = St::SQUARE; t_ = in.now; squares_++; o.square = true; }
            break;
        case St::SQUARE:
            if (in.page_now) { st_ = St::OPEN; open_t_ = in.now; opens_++; o.opened = true; o.read_live = true; }
            else if (in.now - t_ >= cfg_.open_wait_ms) {
                if (squares_ < cfg_.tries) { st_ = St::WAIT; room_seen_ = false; o.retry = true; }
                else { st_ = St::FALLBACK; o.fallback = true; }
            }
            break;
        case St::OPEN:
            o.read_live = true;
            if (!in.page_now && in.room_now) { st_ = St::BACK; back_t_ = in.now; o.closed = true; }
            break;
        case St::BACK:
            if (in.page_now) { st_ = St::OPEN; o.read_live = true; break; }   // it was a flicker: still open
            if (!in.room_now) { back_t_ = in.now; break; }                     // a notice box or a transition: wait for the room page
            if (in.now - back_t_ < cfg_.back_ms) break;
            if (back_t_ - open_t_ < cfg_.min_open_ms && opens_ < cfg_.tries) { st_ = St::WAIT; room_seen_ = false; o.retry = true; break; }
            st_ = St::DONE; o.lock = true;
            break;
        default: break;
        }
        return o;
    }

private:
    Cfg cfg_;
    St st_ = St::OFF;
    uint64_t t0_ = 0, t_ = 0, room_t_ = 0, open_t_ = 0, back_t_ = 0;
    int squares_ = 0, opens_ = 0;
    bool room_seen_ = false;
};

}  // namespace jsps
