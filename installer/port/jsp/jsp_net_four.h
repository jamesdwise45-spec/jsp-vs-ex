// jsp_net_four.h -- ENHANCEMENT (4-player online, piece 3): the game glue's rules for 2..4 humans in one native Friendly Match room, as pure code
// (no Windows, no game memory) so installer/tools/net_four_test.cpp runs them on the host. jsp_net_game.cpp, jsp_net_native.cpp and
// jsp_net_native_fight.cpp call them. Everything here is gated by JSP_ROLLBACK_MAX_PLAYERS (2..4, default 2): with 2 every rule returns today's answer.
//
//   - pairing: how many rollback players a native room of `members` makes (link_players), and when an open link must be planned again (replan);
//   - the slot claim: this PC's battle slot is its room record index (the online mode ctor 0x0F7880 stores it at share pad +0x6E0 when the fight
//     loads, too late for the rendezvous in the room), found by this PC's room member id in the 4 fighter records R+0x68+0x24*i (+0x08 member id,
//     +0x1A COM flag); room_claim says whether the records are settled (as many humans as members, packed in records 0..n-1);
//   - the mesh: make_mesh_inner builds the N-peer HELO/PLN2 (jsp_net_session.h DirectRendezvous) from the re-psn mesh start (jsp_net_repsn.h MeshStart);
//     every PC takes a peer's endpoint from ITS OWN re-psn view (mesh_resolver), never the host's view in PLN2 when they differ (two players in one
//     house: re-psn hands them each other's LAN address, the host elsewhere sees their public one);
//   - the leaver: a disconnect with 3-4 peers ends the match only when nobody else is left (leave_policy); its text;
//   - the takeover (jsp_net_native_fight.h jnf::take_human): GekkoNet slot r = room record r for every human record of the room; the fighter's net slot
//     is NOT r once teams are split (the builder numbers team 0 first), so the pad take maps net slot -> record by member id (jnf::net_ports, FIX swapped controls);
//   - the teams: the room's own, as the players chose them (nothing here writes them). FIX (test13): after a Select character / Fight again the
//     roster write keeps them too (jspk::ROOM in jsp_net_pick.h: kept when a valid 2 + 2, else the nearest valid layout), for 2..4 players.
// JSP_NET_FOUR_RULES_ONLY: everything but the mesh part, without the network headers.
#pragma once
#ifndef JSP_NET_FOUR_RULES_ONLY
#include "jsp_net_repsn.h"
#endif

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace jsp4 {

// JSP_ROLLBACK_MAX_PLAYERS: 2..4, anything else (unset, 0, 5, text) is 2 = two players only (today's behaviour).
inline int max_players(const char* setting)
{
    if (!setting || !*setting) return 2;
    const int v = std::atoi(setting);
    return v < 2 || v > 4 ? 2 : v;
}

// The rollback players of a native room of `members` (the game's own room, ps3_np_matching2_room) plus `bots` TEST bots (JSP_NET_TEST_BOTS, see
// jsp_net_game.cpp; 0 in play). 0 = no rollback link (the fight stays on the game's own netcode). max = max_players(). With max 2 and no bot: exactly
// 2 members, as before. A room with more humans than the setting allows gets no link at all (a rollback mesh that leaves one human out cannot run).
inline int link_players(int members, int bots, int max)
{
    if (max < 2 || max > 4) max = 2;
    if (bots < 0) bots = 0;
    const int n = members + bots;
    if (members < (bots ? 1 : 2) || n < 2 || n > max) return 0;
    if (max == 2 && (bots || members != 2)) return members == 2 && !bots ? 2 : 0;
    return n;
}

// Between matches an open link of `planned` players whose room now makes `now` players (link_players, 0 = no link) is closed and opened again with the
// new count; during a running match nothing changes here (GekkoNet's disconnect handles a leaver, see leave_policy). Two players (max 2): never,
// as before (the 2-player link closes only when the room is left or has fewer than 2 members).
inline bool replan(int planned, int now, bool running, int max)
{
    if (max <= 2 || running) return false;
    return now != planned;
}

// The room's fighter records as the glue reads them (R+0x68+0x24*i: +0x08 u16 member id, 0 = COM; +0x1A COM flag).
struct RoomView {
    uint16_t member[4] = {0, 0, 0, 0};
    bool com[4] = {true, true, true, true};
};
inline uint8_t human_mask(const RoomView& r)
{
    uint8_t m = 0;
    for (int i = 0; i < 4; i++) if (!r.com[i] && r.member[i]) m |= (uint8_t)(1u << i);
    return m;
}
inline int popcount4(uint8_t m) { int n = 0; for (int i = 0; i < 4; i++) n += (m >> i) & 1; return n; }

// Ok: *slot = the record index (exact claim). Rank: *slot = kRankSlot + my member id (DirectRendezvous::claim_rank: the host hands out slots in member-id
// order). Unknown: this PC's room member id is not readable yet.
enum class Claim { Ok, Rank, Unknown };
constexpr int kRankSlot = 0x100;
inline const char* claim_text(Claim c) { return c == Claim::Ok ? "the room record" : c == Claim::Rank ? "the member order" : "this player's room member id is not known yet"; }
// This PC's claim. humans = the room's human members (the native member count). GekkoNet slots are 0..N-1.
// The records are exact only when they hold this PC and exactly `humans` humans packed in records 0..humans-1 (the last fight's placement of this same room).
// FIX (a 4-copy run + review): otherwise (before the first fight they are all zero; after a leave they still show the leaver, or a gap 0/1/3) the claim is the
// member order: room member ids count up in join order and the host places humans in that order (owner record 0, first joiner record 1 in every trace; that run:
// members 1-4 = records 0-3). UNVERIFIED after a leave (does the host repack 1, 2, 4 into records 0-2?): a wrong slot is caught at the session start
// (battle slot != rollback slot: refused with the reason on screen; the link is then planned again from the records that fight filled).
inline Claim room_claim(const RoomView& r, uint16_t my_member, int humans, int* slot)
{
    *slot = -1;
    if (!my_member) return Claim::Unknown;
    int mine = -1;
    for (int i = 0; i < 4; i++) if (!r.com[i] && r.member[i] == my_member) mine = i;
    const uint8_t m = human_mask(r);
    if (mine >= 0 && humans >= 1 && humans <= 4 && popcount4(m) == humans && m == (uint8_t)((1u << humans) - 1)) { *slot = mine; return Claim::Ok; }
    *slot = kRankSlot + my_member; return Claim::Rank;
}

#ifndef JSP_NET_FOUR_RULES_ONLY
// ---- the mesh (piece 2 MeshStart -> piece 1 DirectRendezvous) ----
// tag (re-psn user id) -> THIS PC's view of that member's endpoint. Used for the other clients of a PLN2: the host's view of them may be another address.
inline std::function<bool(uint32_t, jspnet::Endpoint*)> mesh_resolver(const std::vector<jspnet::PsnrMember>& members, uint32_t self_user)
{
    return [members, self_user](uint32_t tag, jspnet::Endpoint* out) {
        if (!tag || tag == self_user) return false;
        for (const jspnet::PsnrMember& m : members) if (m.user == tag && m.ep.port) { *out = m.ep; return true; }
        return false;
    };
}
// The N-peer HELO/PLN2 of a re-psn mesh. claim = this PC's battle slot (0..3; < 0 = no claim: slots by peer order). The owner hosts, the session id is the
// room id; the host takes a HELO only from the room's other members (its own view of them); every peer carries its re-psn user id as the tag.
// nullptr: the owner is not among the members (cannot happen once the mesh started).
inline jspnet::DirectRendezvous* make_mesh_inner(jspnet::UdpMux& mux, jspnet::MessageHub& hub, const jspnet::RePsnRendezvous::MeshStart& s, int claim)
{
    const jspnet::PsnrMember* owner = nullptr;
    for (const jspnet::PsnrMember& m : s.members) if (m.user == s.owner_user) owner = &m;
    jspnet::DirectRendezvous* d = nullptr;
    if (s.host) {
        d = new jspnet::DirectRendezvous(mux, hub, s.local_count, s.session_id);
        d->set_expected_peers((uint8_t)s.members.size());
        for (const jspnet::PsnrMember& m : s.members) if (m.user != s.self_user) d->add_only_peer(m.ep);
    } else {
        if (!owner) return nullptr;
        d = new jspnet::DirectRendezvous(mux, hub, s.local_count, owner->ep);
        d->set_resolver(mesh_resolver(s.members, s.self_user));
    }
    if (claim >= 0 && claim < 4) d->claim_slot((uint8_t)claim);
    else if (claim >= kRankSlot) d->claim_rank((uint16_t)(claim - kRankSlot));   // the member order (room_claim)
    d->set_tag(s.self_user);
    return d;
}
// One log line per link: "Bob direct, Carol relay"
inline std::string links_text(const std::vector<jspnet::RePsnRendezvous::LinkInfo>& links)
{
    std::string s;
    for (const auto& l : links) s += (s.empty() ? "" : ", ") + l.name + " " + jspnet::path_name(l.path) + " (" + jspnet::to_string(l.ep) + ")";
    return s.empty() ? std::string("-") : s;
}
#endif  // JSP_NET_FOUR_RULES_ONLY

// ---- the leaver ----
enum class Leave { EndMatch, GoOn };
// GekkoNet fired Disconnected for one slot. multi = 3-4 peers in the plan (and the setting allows them); connected = the remote peers whose slots are
// not all gone. The match goes on (the leaver's fighter gets GekkoNet's agreed neutral input on every PC) while someone else is still there.
// gone = the remote slots GekkoNet reported gone so far (stats().gone_mask). FIX (review: a cut link, A-D only): GekkoNet's disconnect claims then make B and C drop BOTH A
// and D while A and D keep B and C, so the PCs would simulate different inputs: only ONE player gone in the whole match goes on; a second one ends the match on
// every PC that sees it ("Connection lost"), and each PC that ends drops out for the others, so the split never runs on.
inline Leave leave_policy(bool multi, unsigned connected, unsigned gone = 1) { return multi && connected >= 1 && gone == 1 ? Leave::GoOn : Leave::EndMatch; }
inline std::string left_text(int handle) { return "Player " + std::to_string(handle + 1) + " left"; }

// ---- a fight whose human count is not the plan's (test10 friends: a link opened for 2, a fight of 3) ----
// The on-screen line when a session is refused because the fight has `humans` human players and the link was planned for `planned` (max = max_players()).
// Starts with "Rollback off" (jsp_net_game.cpp shows it as it is, without the "could not start" prefix). The fight itself goes on, on the game's own connection.
inline std::string count_text(int humans, int planned, int max)
{
    if (humans > max)
        return "Rollback off: this fight has " + std::to_string(humans) + " players and rollback is set to at most " + std::to_string(max) +
               " (JSP_ROLLBACK_MAX_PLAYERS). The game's own connection is used.";
    return "Rollback off for this fight: it has " + std::to_string(humans) + " players, the rollback link was set up for " + std::to_string(planned) +
           ". It is set up again for " + std::to_string(humans) + " after this fight.";
}
// FIX (review R2): the fight's human records have a gap (e.g. records 0, 1 and 3 after a leave) or lie outside the plan's slots. GekkoNet slot s is room record s,
// so no rollback plan of these players can serve them until the game packs them into places 1..N again (it may, at the next fight setup).
inline std::string gap_text(uint8_t humans)
{
    std::string places;
    for (int i = 0; i < 4; i++) if (humans >> i & 1) places += (places.empty() ? "" : ", ") + std::to_string(i + 1);
    return "Rollback off for this fight: the players are in places " + (places.empty() ? std::string("-") : places) +
           " of the room, and rollback needs them in places 1 to N without a gap. The game's own connection is used.";
}

}  // namespace jsp4
