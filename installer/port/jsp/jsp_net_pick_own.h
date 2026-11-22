// jsp_net_pick_own.h -- FIX (pick7; post-game / pick, JSP_ROLLBACK_PICK=1): the after-match menu against the rules it sits between, as pure code
// (no Windows, no game memory). jsp_net_game.cpp and jsp_net_native.cpp call these; installer/tools/net_pick_drive_test.cpp and
// net_pick_match_test.cpp run them on the host.
//   seats        the planned rollback seats (jsp_native_fight_map's contract since the review of PR 248: bit r = room record / rollback slot r,
//                (1 << total_players) - 1). The start of EVERY session runs the fighter map with them, the one after a Select character
//                included; the menu opens on a link only when its players' battle slots are exactly those seats.
//   predict_map  the fighter map that session start will build from a room image after the roster write (the builder's placement, then
//                jnf::net_ports with the seats), so the builder hook can log it before the fight and the host test can check own pad ->
//                own fighter after a Select character.
//   end_rule     one test of a normal match end (jkr::normal_end) for the three things that hang on it: the share pad watchdog's flag (set
//                before the disarm that consumes it), the prompt, and the re-arm's "last end ok".
#pragma once
#include <cstdint>
#include <string>
#include "jsp_net_ko_rule.h"
#include "jsp_net_native_fight.h"
#include "jsp_net_pick.h"

namespace jspo {

// ---- seats ----
inline uint32_t plan_seats(int total_players) { return total_players >= 1 && total_players <= 4 ? (1u << total_players) - 1u : 0u; }
// The players' battle slots (room records) as a seat mask; 0 when one is out of range or two are the same.
inline uint32_t slot_seats(const uint8_t* slots, int n)
{
    uint32_t s = 0;
    if (!slots || n < 1 || n > 4) return 0;
    for (int i = 0; i < n; i++) { if (slots[i] > 3 || (s & (1u << slots[i]))) return 0; s |= 1u << slots[i]; }
    return s;
}
// The menu's link: n players, every one on a planned seat, every planned seat taken.
inline bool seats_ok(const uint8_t* slots, int n, int total_players) { return n == total_players && n >= 2 && slot_seats(slots, n) == plan_seats(total_players); }

// ---- the fighter map of the coming session ----
struct Map {
    jnf::MapErr err = jnf::MAP_MISSING;
    int8_t port[jnf::kRecCount] = { -1, -1, -1, -1 };   // net slot -> rollback slot (room record), as jsp_native_fight_map fixes it
    int net_of_rec[4] = { -1, -1, -1, -1 };             // the builder's net slot of each record (-1: a COM)
};
// The builder places one battle player per record (jnf::placement_net_slot: team 0, then team 1, each in record order; a COM gets none) with
// that record's member id; the session start then maps them against the room's human records and the seats (jnf::net_ports).
inline Map predict_map(const uint8_t img[jspk::kRoomSize], uint32_t seats)
{
    Map m;
    uint8_t team[4], com[4]; uint16_t rmem[4];
    int32_t net[jnf::kPlayers]; uint16_t pmem[jnf::kPlayers];
    for (int r = 0; r < 4; r++) {
        const uint8_t* q = img + jspk::kOffRecords + jspk::kRecSize * (uint32_t)r;
        team[r] = q[jspk::kRecTeam]; com[r] = q[jspk::kRecCom] ? 1 : 0; rmem[r] = jspk::be16(q + jspk::kRecMember);
    }
    for (uint32_t i = 0; i < jnf::kPlayers; i++) { net[i] = -1; pmem[i] = 0; }
    for (int r = 0; r < 4; r++) { m.net_of_rec[r] = jnf::placement_net_slot(team, com, r); net[r] = m.net_of_rec[r]; pmem[r] = rmem[r]; }
    m.err = jnf::net_ports(net, pmem, rmem, com, seats, m.port);
    return m;
}
// This PC's pad (rollback slot self_slot) drives its own fighter: the map is ok and the fighter placed for its record takes its port.
inline bool own_ok(const Map& m, int self_slot)
{
    if (m.err != jnf::MAP_OK || self_slot < 0 || self_slot > 3) return false;
    const int n = m.net_of_rec[self_slot];
    return n >= 0 && m.port[n] == self_slot;
}

// ---- the match end ----
struct End {
    bool wd_flag = false;       // jsp_native_fight_end_normal() before the disarm: the share pad watchdog may act on the exit
    bool prompt = false;        // the after-match menu may open (Flow::match_end)
    bool last_end_ok = false;   // the native re-arm waits 300 ms instead of 2 s and does not count a bad end
};
inline End end_rule(const std::string& text)
{
    const bool normal = jkr::normal_end(text);
    End e; e.wd_flag = normal; e.prompt = normal; e.last_end_ok = normal || text == "Match ended";
    return e;
}

}  // namespace jspo
