// jsp_net_menu4.h -- ENHANCEMENT (test13, the after-match menu for 3-4 players; JSP_ROLLBACK_PICK_MULTI=1, default 0): the glue between a
// rollback link's plan (jsp_net_session.h SessionPlan) and the after-match menu (jsp_net_pick_flow.h Flow), as pure code (no Windows, no game
// memory), so installer/tools/net_pick_flow_test.cpp and net_pick_drive_test.cpp run it on the host. jsp_net_game.cpp calls these.
//   menu_seats   which player this PC is, every player's battle slot and the peers every menu message goes to. Player index = peer id (the
//                Flow's route() checks a message's player index against its sender's peer id). The battle slot is NOT the peer id with 3-4
//                players: the field logs show "slot 2 peer 1", "slot 3 peer 2" (the slot claim = the room record index, jsp_net_four.h). So
//                slots[peer] = that peer's first_slot, for this PC (local_peer / local_first) and every remote.
//                2 players: exactly the mapping of test12 and before (me = host ? 0 : 1, the one remote at me ^ 1, sent to peer host ? 1 : 0),
//                whatever the switch says. 3-4 players: only with JSP_ROLLBACK_PICK_MULTI=1, one human per PC, peer ids exactly 0..n-1, and the
//                slots exactly the planned rollback seats (jspo::seats_ok, the fighter map's contract).
//   leave rule   one of 3-4 players leaves at the menu (its Leave, or its idle timeout): that PC closes the link and gives the room up
//                (Act::LeaveGiveUp, as with 2 players). Every other PC closes the link WITHOUT giving the room up (Act::LeaveClose,
//                "A player left: back to the room"), goes back to the room, and the room goes on. The native room pairing plans a new link
//                for the players who stay, but only once the leaver is out of the room: while the room still has the old player count
//                (the leaver has not pressed Circle yet), a new plan would wait for a PC that no longer joins and use up the room's 3 tries
//                (then no rollback in this room at all). So these PCs hold the re-plan (leave_gate) until the room's count drops below the
//                link's, or kLeaveWaitMs passed (then the plan is made as before). A re-plan that follows the drop starts with fresh tries.
//                4 -> 3 players: a 3-player link, the menu again; 3 -> 2: the 2-player path. The same hold follows Act::PeerFroze (another
//                PC's game froze or its window closed at the menu: that process is gone or going) and Act::Froze (this PC's own game froze
//                and came back: it holds like the others, so all of them plan again together; review REVIEW-MENU4 2). A lost link (silence:
//                every PC decides R_LINK_LOST on its own, review 1) or a lost pick round does not hold: nobody chose to leave, so the re-plan
//                runs at once, as today. The hold ends on ANY change of the room's player count (review 3: a join during the hold must not
//                keep it the whole time), or after kLeaveWaitMs; a player who gave the room up but stayed in it is still counted then.
#pragma once
#include <cstddef>
#include <cstdint>
#include "jsp_net_pick_flow.h"
#include "jsp_net_pick_own.h"

namespace jspm4 {

inline constexpr const char* kKey = "JSP_ROLLBACK_PICK_MULTI";
// exactly "1" opens the menu on links of 3-4 players; anything else (unset, 0, junk) = 2 players only, as before
inline bool key_on(const char* v) { return v && v[0] == '1' && !v[1]; }

enum class Why : uint8_t { Ok = 0, NotTwo, MultiOff, Shape, PeerIds, Seats };
inline const char* why_text(Why w)
{
    switch (w) {
    case Why::Ok: return "ok";
    case Why::NotTwo: return "the plan is not 2 players on the planned rollback seats 0 and 1";
    case Why::MultiOff: return "JSP_ROLLBACK_PICK_MULTI is not 1 (every PC needs it for 3-4 players)";
    case Why::Shape: return "the plan is not one player per PC";
    case Why::PeerIds: return "the peer ids are not 0..n-1, each once";
    case Why::Seats: return "the players' battle slots are not the planned rollback seats";
    }
    return "?";
}

struct Seats {
    bool ok = false;
    Why why = Why::NotTwo;
    int me = 0, n = 0;                        // this PC's player index (= its peer id) and the players
    uint8_t slots[jspk::kMaxPlayers] = {};    // player i's battle slot (room record index)
    uint8_t to[jspk::kMaxPlayers - 1] = {};   // the peer ids every menu message goes to
    int nto = 0;
};

// P: SessionPlan or a test's look-alike (host, total_players, local_peer, local_first, local_count, remotes[] of {peer, first_slot, count}).
template <class P> Seats menu_seats(const P& plan, bool multi)
{
    Seats s;
    const size_t nr = plan.remotes.size();
    if (plan.total_players == 2 && nr == 1) {   // 2 players: test12's mapping, unchanged (the peer ids are not read)
        s.n = 2; s.me = plan.host ? 0 : 1;
        s.slots[s.me] = plan.local_first; s.slots[s.me ^ 1] = plan.remotes[0].first_slot;
        s.to[0] = (uint8_t)(plan.host ? 1 : 0); s.nto = 1;
        s.ok = jspo::seats_ok(s.slots, 2, (int)plan.total_players);
        s.why = s.ok ? Why::Ok : Why::NotTwo;
        return s;
    }
    if (plan.total_players < 3 || plan.total_players > jspk::kMaxPlayers) { s.why = Why::NotTwo; return s; }
    if (!multi) { s.why = Why::MultiOff; return s; }
    const int n = plan.total_players;
    if (nr != (size_t)(n - 1) || plan.local_count != 1) { s.why = Why::Shape; return s; }
    for (size_t i = 0; i < nr; i++) if (plan.remotes[i].count != 1) { s.why = Why::Shape; return s; }
    uint32_t seen = 0;
    auto take = [&](int peer) { if (peer < 0 || peer >= n || (seen & (1u << peer))) return false; seen |= 1u << peer; return true; };
    if (!take(plan.local_peer) || (plan.local_peer == 0) != plan.host) { s.why = Why::PeerIds; return s; }
    for (size_t i = 0; i < nr; i++) if (!take(plan.remotes[i].peer)) { s.why = Why::PeerIds; return s; }
    s.n = n; s.me = plan.local_peer;
    s.slots[s.me] = plan.local_first;
    for (size_t i = 0; i < nr; i++) { s.slots[plan.remotes[i].peer] = plan.remotes[i].first_slot; s.to[s.nto++] = plan.remotes[i].peer; }
    s.ok = jspo::seats_ok(s.slots, n, n);
    s.why = s.ok ? Why::Ok : Why::Seats;
    return s;
}

// ---- the leave rule (see the top) ----
inline constexpr uint64_t kLeaveWaitMs = 180000;   // = the after-match menu's own idle leave (jspg::Config idle_ms default)
// The menu closed this PC's link because another player chose to go (its Leave or its idle timeout) or its game froze / closed, on a link of n > 2.
inline bool leave_wait_applies(int n, jspf::Act a, jspg::Reason r)
{
    return n > 2 && (a == jspf::Act::PeerFroze || a == jspf::Act::Froze || (a == jspf::Act::LeaveClose && r == jspg::R_PEER_LEFT));
}
struct LeaveWait { uint64_t room = 0; int players = 0; uint64_t until = 0; };
inline LeaveWait leave_wait(uint64_t room, int players, uint64_t now) { LeaveWait w; w.room = room; w.players = players; w.until = now + kLeaveWaitMs; return w; }
enum class Gate : uint8_t { None = 0, Hold, Release };
// native_poll, before it plans a link for `room` with `want` players: Hold = not yet; Release = the wait is over (the count changed, or the
// time ran out): plan now, with the room's tries counted afresh; None = no wait (another room forgets it).
inline Gate leave_gate(LeaveWait& w, uint64_t room, int want, uint64_t now)
{
    if (!w.room) return Gate::None;
    if (room != w.room) { w = LeaveWait(); return Gate::None; }
    if (want == w.players && now < w.until) return Gate::Hold;
    w = LeaveWait();
    return Gate::Release;
}

}  // namespace jspm4
