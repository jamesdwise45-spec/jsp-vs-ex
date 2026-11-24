// net_four_test.cpp -- ENHANCEMENT (4-player online, piece 3): host test of the game glue's 2..4 player rules (installer/port/jsp/jsp_net_four.h) and of the
// mesh factory over REAL loopback UDP: room pairing and re-planning (JSP_ROLLBACK_MAX_PLAYERS 2 = today's answers), the slot claim from the room records,
// the N-peer HELO/PLN2 built by make_mesh_inner with permuted claims, a clash, the same-house case (a client takes another client's endpoint from its OWN
// re-psn view, not the host's), the leaver rule and the takeover's human slots (the 3-4 human team rule comes with the after-match menu). Build and run: installer/tools/net_four_test.sh.
#include "jsp_net_four.h"
#include "jsp_net_native_fight.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

using namespace jspnet;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// One PC of the mesh test: its socket, hub and the rendezvous make_mesh_inner built for it.
struct Pc {
    UdpMux mux; std::unique_ptr<MessageHub> hub; std::unique_ptr<DirectRendezvous> rz; int r = 0; uint32_t user = 0;
    Endpoint ep(const char* ip = "127.0.0.1") const { Endpoint e; char b[40]; std::snprintf(b, sizeof b, "%s:%u", ip, (unsigned)mux.local_port()); parse_endpoint(b, e); return e; }
};
// n PCs, user ids 11.., PC 0 owns the room. view(i, j) = PC i's re-psn view of PC j's endpoint. claims[i] = PC i's battle slot (-1 none).
struct MeshRun {
    Pc pc[4]; int n;
    MeshRun(int count, const int* claims, std::function<Endpoint(int, int)> view = nullptr) : n(count)
    {
        for (int i = 0; i < n; i++) { std::string err; CHECK(pc[i].mux.open(0, &err)); pc[i].hub.reset(new MessageHub(pc[i].mux)); pc[i].user = 11 + (uint32_t)i; }
        for (int i = 0; i < n; i++) {
            RePsnRendezvous::MeshStart s; s.host = i == 0; s.session_id = 777; s.local_count = 1; s.self_user = pc[i].user; s.owner_user = pc[0].user;
            for (int j = 0; j < n; j++) { PsnrMember m; m.user = pc[j].user; m.member_id = (uint16_t)(j + 1); m.ep = view ? view(i, j) : pc[j].ep(); s.members.push_back(m); }
            pc[i].rz.reset(jsp4::make_mesh_inner(pc[i].mux, *pc[i].hub, s, claims ? claims[i] : -1));
        }
    }
    bool run(int steps = 400)
    {
        for (int k = 0; k < steps; k++) {
            bool all = true;
            for (int i = 0; i < n; i++) { pc[i].mux.pump(); pc[i].hub->pump(); if (!pc[i].r) pc[i].r = pc[i].rz->poll(); all = all && pc[i].r; }
            if (all) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    const PeerSlots* remote(int i, uint32_t user) const { for (const PeerSlots& p : pc[i].rz->plan().remotes) if (p.tag == user) return &p; return nullptr; }
};

int main()
{
    // ---- pairing: JSP_ROLLBACK_MAX_PLAYERS ----
    CHECK(jsp4::max_players(nullptr) == 2 && jsp4::max_players("") == 2 && jsp4::max_players("4") == 4 && jsp4::max_players("3") == 3);
    CHECK(jsp4::max_players("5") == 2 && jsp4::max_players("1") == 2 && jsp4::max_players("x") == 2 && jsp4::max_players("0") == 2);
    // default 2: exactly 2 members, as before; 3-4 members get no link (no 2-player takeover of a bigger fight)
    for (int m = 0; m <= 5; m++) CHECK(jsp4::link_players(m, 0, 2) == (m == 2 ? 2 : 0));
    CHECK(jsp4::link_players(2, 1, 2) == 0);   // a bot never widens a 2-player setting
    // 4: 2..4 members; more than the setting allows: none
    CHECK(jsp4::link_players(2, 0, 4) == 2 && jsp4::link_players(3, 0, 4) == 3 && jsp4::link_players(4, 0, 4) == 4 && jsp4::link_players(1, 0, 4) == 0);
    CHECK(jsp4::link_players(4, 0, 3) == 0 && jsp4::link_players(3, 0, 3) == 3 && jsp4::link_players(2, 2, 4) == 4 && jsp4::link_players(3, 2, 4) == 0);
    // re-plan: only with 3-4 allowed, only outside a running match, only when the count changed (a join before the fight, a leaver between matches)
    CHECK(!jsp4::replan(2, 3, false, 2) && !jsp4::replan(2, 0, false, 2));
    CHECK(jsp4::replan(2, 3, false, 4) && jsp4::replan(4, 3, false, 4) && jsp4::replan(3, 2, false, 4) && jsp4::replan(2, 0, false, 4));
    CHECK(!jsp4::replan(4, 3, true, 4) && !jsp4::replan(4, 4, false, 4));

    // ---- the slot claim: the room records when they are exact, else the member order (a rank claim) ----
    { jsp4::RoomView r; int s = 9; const int R = jsp4::kRankSlot;
      CHECK(jsp4::room_claim(r, 0, 3, &s) == jsp4::Claim::Unknown && s == -1);            // my member id not read yet
      CHECK(jsp4::room_claim(r, 2, 2, &s) == jsp4::Claim::Rank && s == R + 2);            // records still empty (before the first fight)
      r.member[0] = 1; r.com[0] = false; r.member[1] = 2; r.com[1] = false; r.member[2] = 3; r.com[2] = false;
      CHECK(jsp4::room_claim(r, 3, 3, &s) == jsp4::Claim::Ok && s == 2);                  // exact: 3 humans packed, mine in record 2
      CHECK(jsp4::room_claim(r, 2, 4, &s) == jsp4::Claim::Rank && s == R + 2);            // a 4th member is in the room, not in the records yet
      r.member[3] = 4; r.com[3] = false;
      CHECK(jsp4::room_claim(r, 4, 4, &s) == jsp4::Claim::Ok && s == 3 && jsp4::human_mask(r) == 0xF);
      CHECK(jsp4::room_claim(r, 4, 3, &s) == jsp4::Claim::Rank && s == R + 4);            // 4 -> 3: the last fight's records still hold the leaver
      r.com[2] = true; r.member[2] = 0;                                                     // records 0, 1, 3 (a gap): never Gap forever, the member order
      CHECK(jsp4::room_claim(r, 4, 3, &s) == jsp4::Claim::Rank && s == R + 4 && jsp4::human_mask(r) == 0xB);
      CHECK(std::strlen(jsp4::claim_text(jsp4::Claim::Rank)) > 0); }
    // rank claims over the real HELO/PLN2: member ids {1, 2, 4} (3 left) -> slots 0, 1, 2 in member order, whatever the join order
    { const int claims[3] = {jsp4::kRankSlot + 1, jsp4::kRankSlot + 4, jsp4::kRankSlot + 2}; MeshRun m(3, claims); CHECK(m.run());
      for (int i = 0; i < 3; i++) CHECK(m.pc[i].r == 1 && m.pc[i].rz->plan().total_players == 3);
      CHECK(m.pc[0].rz->plan().local_first == 0 && m.pc[1].rz->plan().local_first == 2 && m.pc[2].rz->plan().local_first == 1); }
    // a mix: one exact claim (slot 0) and rank claims fill the rest in member order; equal rank keys clash
    { const int claims[4] = {0, jsp4::kRankSlot + 9, jsp4::kRankSlot + 3, jsp4::kRankSlot + 5}; MeshRun m(4, claims); CHECK(m.run());
      CHECK(m.pc[0].rz->plan().local_first == 0 && m.pc[2].rz->plan().local_first == 1 && m.pc[3].rz->plan().local_first == 2 && m.pc[1].rz->plan().local_first == 3); }
    { const int claims[3] = {jsp4::kRankSlot + 1, jsp4::kRankSlot + 2, jsp4::kRankSlot + 2}; MeshRun m(3, claims); CHECK(m.run());
      for (int i = 0; i < 3; i++) CHECK(m.pc[i].r == -1); }

    // ---- the mesh: 4 PCs, claims permuted (room records: owner 0, Carol 1, Bob 2, Dan 3) ----
    { const int claims[4] = {0, 2, 1, 3}; MeshRun m(4, claims); CHECK(m.run());
      for (int i = 0; i < 4; i++) {
          const SessionPlan& p = m.pc[i].rz->plan();
          CHECK(m.pc[i].r == 1 && p.total_players == 4 && p.local_first == claims[i] && p.local_count == 1 && p.remotes.size() == 3 && p.session_id == 777);
          CHECK(p.host == (i == 0) && p.roster_mask() == 0xF);
          for (int j = 0; j < 4; j++) if (j != i) { const PeerSlots* r = m.remote(i, m.pc[j].user); CHECK(r && r->first_slot == claims[j] && r->ep == m.pc[j].ep()); }
          const auto mk = p.peer_slot_mask(); unsigned all = 0; for (int k = 0; k < 4; k++) { CHECK((mk[k] & all) == 0); all |= mk[k]; } CHECK(all == 0xF);
      }
      CHECK(m.pc[1].rz->plan().local_peer != 0 && m.pc[0].rz->plan().local_peer == 0); }
    // 3 PCs (one left between matches): slots 0..2
    { const int claims[3] = {0, 1, 2}; MeshRun m(3, claims); CHECK(m.run());
      for (int i = 0; i < 3; i++) CHECK(m.pc[i].r == 1 && m.pc[i].rz->plan().total_players == 3 && m.pc[i].rz->plan().local_first == i && m.pc[i].rz->plan().roster_mask() == 0x7); }
    // two PCs claim one slot: every PC fails with the clash (no fight starts with two PCs driving one fighter)
    { const int claims[4] = {0, 1, 1, 3}; MeshRun m(4, claims); CHECK(m.run());
      for (int i = 0; i < 4; i++) CHECK(m.pc[i].r == -1 && m.pc[i].rz->error().find("slot claims clash") != std::string::npos); }
    // a claim past the players (records 0, 1, 3 with 3 humans): refused, never a plan with a gap
    { const int claims[3] = {0, 1, 3}; MeshRun m(3, claims); CHECK(m.run());
      CHECK(m.pc[0].r == -1); }

    // ---- the same house: Bob (1) and Carol (2) share a public address; re-psn hands them each other's LAN address (here 127.0.0.2), the host sees the other.
    // Bob's plan must hold Carol at 127.0.0.2 (his own view), not the host's 127.0.0.1 from PLN2; and a datagram to it reaches Carol. ----
    { const int claims[4] = {0, 1, 2, 3};
      MeshRun m(4, nullptr);   // opens the 4 sockets (their ports make the views); the rendezvous are rebuilt below with the same-house view
      for (int i = 0; i < 4; i++) m.pc[i].rz.reset();
      for (int i = 0; i < 4; i++) {
          RePsnRendezvous::MeshStart s; s.host = i == 0; s.session_id = 778; s.local_count = 1; s.self_user = m.pc[i].user; s.owner_user = m.pc[0].user;
          for (int j = 0; j < 4; j++) {
              PsnrMember mm; mm.user = m.pc[j].user; mm.member_id = (uint16_t)(j + 1);
              mm.ep = ((i == 1 && j == 2) || (i == 2 && j == 1)) ? m.pc[j].ep("127.0.0.2") : m.pc[j].ep();
              s.members.push_back(mm);
          }
          m.pc[i].hub.reset(new MessageHub(m.pc[i].mux));
          m.pc[i].rz.reset(jsp4::make_mesh_inner(m.pc[i].mux, *m.pc[i].hub, s, claims[i]));
          m.pc[i].r = 0;
      }
      CHECK(m.run());
      for (int i = 0; i < 4; i++) CHECK(m.pc[i].r == 1);
      const PeerSlots* bc = m.remote(1, m.pc[2].user); const PeerSlots* cb = m.remote(2, m.pc[1].user);
      const PeerSlots* hc = m.remote(0, m.pc[2].user); const PeerSlots* db = m.remote(3, m.pc[1].user);
      CHECK(bc && bc->ep == m.pc[2].ep("127.0.0.2") && !(bc->ep == m.pc[2].ep()));   // Bob: Carol by his own view
      CHECK(cb && cb->ep == m.pc[1].ep("127.0.0.2"));                                  // Carol: Bob by hers
      CHECK(hc && hc->ep == m.pc[2].ep() && db && db->ep == m.pc[1].ep());             // the host and Dan keep theirs
      CHECK(m.remote(1, m.pc[0].user)->ep == m.pc[0].ep());                            // the host: as the client asked it (its own view)
      // the resolved endpoint is a working one: Bob -> Carol over it
      const uint8_t hi[3] = {'h', 'i', '!'};
      m.pc[1].mux.send(bc->ep, kTagGekko, hi, 3);
      bool got = false;
      for (int k = 0; k < 200 && !got; k++) {
          m.pc[2].mux.pump(); Endpoint from; std::vector<uint8_t> d; uint64_t ms = 0;
          while (m.pc[2].mux.pop(kTagGekko, from, d, &ms)) got = got || (d.size() == 3 && d[2] == '!');
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      CHECK(got);
      // the resolver alone: a tag of another member -> its endpoint in this view; self, unknown and zero tags keep the host's
      std::vector<PsnrMember> mem(2); mem[0].user = 5; mem[0].ep = m.pc[0].ep(); mem[1].user = 6; mem[1].ep = m.pc[1].ep("127.0.0.2");
      auto rs = jsp4::mesh_resolver(mem, 5); Endpoint o{};
      CHECK(rs(6, &o) && o == m.pc[1].ep("127.0.0.2") && !rs(5, &o) && !rs(7, &o) && !rs(0, &o)); }

    // ---- the room record carries the count of a 3-4 player mesh (review: a client must not join a host's stale 2-player room) ----
    { const uint32_t B = 0x1234abcd;
      CHECK(make_record(B, 0x77) == make_record(B, 0x77, 2) && make_record(B, 0x77, 2).find(";n=") == std::string::npos);   // 2 players: the old record, byte for byte
      CHECK(make_record(B, 0x77, 4) == make_record(B, 0x77) + ";n=4" && !record_matches(make_record(B, 0x77, 4), B, 0x77) && record_matches(make_record(B, 0x77, 3), B, 0x77, 3));
      uint32_t b = 0; uint64_t k = 0; int np = 0;
      CHECK(record_build(make_record(B, 0x77, 4), &b, &k, &np) && b == B && k == 0x77 && np == 4);
      CHECK(record_build(make_record(B, 0x77), &b, &k, &np) && np == 2 && record_build(make_record(B), &b, &k, &np) && k == 0 && np == 2);
      std::vector<PsnrRoom> rooms(2); rooms[0].id = 5; rooms[0].flags = kRoomFlagRollback; rooms[0].external = make_record(B, 0x77); rooms[0].members = 1; rooms[0].max_members = 2;
      rooms[1].id = 6; rooms[1].flags = kRoomFlagRollback; rooms[1].external = make_record(B, 0x77, 4); rooms[1].members = 1; rooms[1].max_members = 4;
      CHECK(filter_rooms(rooms, B, 0x77, 4).size() == 1 && filter_rooms(rooms, B, 0x77, 4)[0].id == 6 && filter_rooms(rooms, B, 0x77).size() == 1 && filter_rooms(rooms, B, 0x77)[0].id == 5);
      CHECK(filter_rooms(rooms, B, 0x77, 3).empty()); }

    // ---- the leaver ----
    CHECK(jsp4::leave_policy(true, 2) == jsp4::Leave::GoOn && jsp4::leave_policy(true, 1) == jsp4::Leave::GoOn);
    CHECK(jsp4::leave_policy(true, 0) == jsp4::Leave::EndMatch && jsp4::leave_policy(false, 1) == jsp4::Leave::EndMatch && jsp4::leave_policy(false, 0) == jsp4::Leave::EndMatch);
    CHECK(jsp4::leave_policy(true, 2, 1) == jsp4::Leave::GoOn && jsp4::leave_policy(true, 1, 2) == jsp4::Leave::EndMatch && jsp4::leave_policy(true, 2, 2) == jsp4::Leave::EndMatch);   // a second player gone (or a cut link): end
    CHECK(jsp4::left_text(2) == "Player 3 left" && jsp4::left_text(0) == "Player 1 left");
    // after the leaver, between matches: 3 humans left of 4 re-plan to 3; 2 left fall back to the 2-player path
    CHECK(jsp4::replan(4, jsp4::link_players(3, 0, 4), false, 4) && jsp4::link_players(3, 0, 4) == 3 && jsp4::link_players(2, 0, 4) == 2);

    // ---- test10 friends (rooms 54/58): a link opened at 2 members, then a 3rd joined; the fight had 3 human records ----
    // with 4 allowed the room is planned again for 3 between fights; with 2 the 2-player link closes and the room of 3 gets none (until it is 2 again)
    CHECK(jsp4::replan(2, jsp4::link_players(3, 0, 4), false, 4) && jsp4::link_players(3, 0, 4) == 3);
    CHECK(jsp4::link_players(3, 0, 2) == 0 && jsp4::link_players(2, 0, 2) == 2);
    { jsp4::RoomView rv; rv.member[0] = 1; rv.member[1] = 2; rv.member[2] = 3; rv.member[3] = 0; rv.com[0] = rv.com[1] = rv.com[2] = false; rv.com[3] = true;   // the bundle's records
      CHECK(jsp4::human_mask(rv) == 0x7 && jsp4::popcount4(jsp4::human_mask(rv)) == 3); }
    CHECK(jsp4::count_text(3, 2, 4) == "Rollback off for this fight: it has 3 players, the rollback link was set up for 2. It is set up again for 3 after this fight.");
    CHECK(jsp4::count_text(3, 2, 2).rfind("Rollback off: this fight has 3 players and rollback is set to at most 2", 0) == 0);
    CHECK(jsp4::count_text(4, 3, 4).rfind("Rollback off for this fight: it has 4 players", 0) == 0);
    // review R2: a gap in the records (0, 1, 3 after a leave) is refused with a plain line (and re-planned after the fight, never the 5-bad-matches give-up)
    { jsp4::RoomView rv; rv.member[0] = 1; rv.member[1] = 2; rv.member[3] = 4; rv.com[0] = rv.com[1] = rv.com[3] = false;
      const uint8_t hm = jsp4::human_mask(rv);
      CHECK(hm == 0xB && (hm & ~((1u << 3) - 1u)) != 0);
      CHECK(jsp4::gap_text(hm) == "Rollback off for this fight: the players are in places 1, 2, 4 of the room, and rollback needs them in places 1 to N without a gap. "
                                  "The game's own connection is used."); }

    // ---- the takeover: battle slot s = GekkoNet slot s for every human record ----
    { uint32_t seen = 0;
      CHECK(jnf::take_human(0, 0xF, &seen) && jnf::take_human(3, 0xF, &seen) && !jnf::take_human(3, 0xF, &seen) && !jnf::take_human(4, 0xF, &seen) && !jnf::take_human(-1, 0xF, &seen));
      seen = 0; CHECK(jnf::take_human(3, 0xB, &seen) && !jnf::take_human(2, 0xB, &seen));   // records 0, 1, 3: the native rule would drop slot 3
      seen = 0; CHECK(!jnf::take_slot(3, 2, &seen)); }

    if (g_fail) { std::printf("%d of %d checks FAILED\n", g_fail, g_n); return 1; }
    std::printf("net four: all %d checks passed\n", g_n);
    return 0;
}
