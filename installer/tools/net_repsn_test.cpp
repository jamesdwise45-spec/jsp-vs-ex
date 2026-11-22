// net_repsn_test.cpp -- host test of the re-psn rendezvous (jsp_net_repsn.h) with a FAKE psnr transport and real loopback UDP (two UdpMux): room list filtering, version
// refusal before joining and at join, roles and player index, punch then the HELO/PLAN start, relay only when allowed and offered; the rendezvous window (a joiner that starts
// before the host keeps reading the room list at the poll interval and joins when the room appears; either side gives up at the window with the text that says what to do;
// the host's room stays listed while it waits; progress every 10 s). Build and run: installer/tools/net_repsn_test.sh.
#include "jsp_net_repsn.h"
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <thread>

using namespace jspnet;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct FakeServer {
    struct Room { PsnrRoom r; std::vector<PsnrMember> members; };
    std::vector<Room> rooms; uint64_t next_room = 100; uint32_t next_member = 1; bool relay = false;
    // ENHANCEMENT (four players): every client of a mesh test (MEMBER_JOINED / MEMBER_LEFT to all members) and the cut links (a PUNCH from first never reaches second)
    std::vector<struct FakeClient*> all; std::set<std::pair<uint32_t, uint32_t>> cut;
    struct FakeClient* find(uint32_t u);
};
struct FakeClient : IPsnrTransport {
    FakeServer& sv; uint32_t user; Endpoint ep; bool is_ready = true;
    std::vector<PsnrEvent> ev; int punches = 0, joins = 0, leaves = 0; FakeClient* peers[2] = { nullptr, nullptr };
    int searches = 0; std::vector<uint64_t> search_at; std::function<uint64_t()> clock;   // every room-list request, at the fake time
    std::string name; bool allow = false; int kicks = 0;   // four players: the online name, the player's relay choice (a relayed member is followed only when allowed)
    struct L { Endpoint ep; bool relay = false, heard = false, acked = false; }; std::map<uint32_t, L> links;
    FakeClient(FakeServer& s, uint32_t u, const Endpoint& e) : sv(s), user(u), ep(e) {}
    void set_link(uint32_t u, const Endpoint& e, bool relay) override {
        L& l = links[u]; l.ep = e; l.relay = l.relay || relay;
        if (relay) if (FakeClient* o = sv.find(u)) if (o->allow) o->links[user].relay = true;   // its first relayed datagram: the real transport follows
    }
    void punch_link(uint32_t u) override {
        punches++; if (sv.cut.count({ user, u })) return;
        if (FakeClient* o = sv.find(u)) { L& l = o->links[user]; l.heard = true; l.acked = l.acked || links[u].heard; }   // our PUNCH says whether we hear it
    }
    unsigned link_seen(uint32_t u) override { auto it = links.find(u); return it == links.end() ? 0u : (it->second.heard ? 1u : 0u) | (it->second.relay ? 2u : 0u) | (it->second.acked ? 4u : 0u); }
    void kick(uint32_t u) override {
        kicks++;
        for (auto& r : sv.rooms) for (size_t i = 0; i < r.members.size(); i++) if (r.members[i].user == u) {
            r.members.erase(r.members.begin() + (long)i); r.r.members--;
            for (FakeClient* p : sv.all) if (p->user != u) { PsnrEvent e; e.kind = PsnrEvent::MemberLeft; e.room_id = r.r.id; e.user = u; p->ev.push_back(e); }
            return;
        }
    }
    bool ready() override { return is_ready; }
    uint32_t self_user() override { return user; }
    bool relay_offered() override { return sv.relay; }
    bool search(std::vector<PsnrRoom>& out) override { searches++; if (clock) search_at.push_back(clock()); out.clear(); for (auto& r : sv.rooms) out.push_back(r.r); return true; }
    void create_room(uint32_t flags, const std::string& external, const std::string& data, uint8_t max) override {
        FakeServer::Room r; r.r.id = sv.next_room++; r.r.flags = flags; r.r.external = external; r.r.owner = user; r.r.members = 1; r.r.max_members = max;
        r.members.push_back(PsnrMember{ user, (uint16_t)sv.next_member++, ep, data, name });
        PsnrEvent e; e.kind = PsnrEvent::RoomJoined; e.room_id = r.r.id; e.owner = user; e.members = r.members; ev.push_back(e);
        sv.rooms.push_back(r);
    }
    void join_room(uint64_t id, const std::string& data) override {
        joins++;
        for (auto& r : sv.rooms) if (r.r.id == id) {
            if (r.r.members >= r.r.max_members) { PsnrEvent e; e.kind = PsnrEvent::Error; e.err = 3; ev.push_back(e); return; }   // ROOM_FULL
            PsnrMember me{ user, (uint16_t)sv.next_member++, ep, data, name }; r.members.push_back(me); r.r.members++;
            PsnrEvent e; e.kind = PsnrEvent::RoomJoined; e.room_id = id; e.owner = r.r.owner; e.members = r.members; ev.push_back(e);
            for (FakeClient* p : peers) if (p && p->user == r.r.owner) { PsnrEvent j; j.kind = PsnrEvent::MemberJoined; j.room_id = id; j.members = { me }; p->ev.push_back(j); }
            for (FakeClient* p : sv.all) if (p != this && p->user != r.r.owner) for (auto& m : r.members) if (m.user == p->user) { PsnrEvent j; j.kind = PsnrEvent::MemberJoined; j.room_id = id; j.members = { me }; p->ev.push_back(j); }
        }
    }
    bool poll_event(PsnrEvent& e) override { if (ev.empty()) return false; e = ev.front(); ev.erase(ev.begin()); return true; }
    void send_punch(const Endpoint&) override { punches++; }
    void leave_room() override { leaves++; }
};
FakeClient* FakeServer::find(uint32_t u) { for (FakeClient* c : all) if (c->user == u) return c; return nullptr; }
// ENHANCEMENT (four players): the N-peer HELO/PLAN stood in for (piece 1 builds the real one): a plan two polls after the links are up
struct FakeInner : IRendezvous {
    RePsnRendezvous::MeshStart m; SessionPlan p; std::string err; int polls = 0;
    explicit FakeInner(const RePsnRendezvous::MeshStart& s) : m(s) {}
    int poll() override { if (++polls < 3) return 0; p.host = m.host; p.session_id = m.session_id; p.total_players = (uint8_t)m.members.size(); return 1; }
    const SessionPlan& plan() const override { return p; }
    const std::string& error() const override { return err; }
    MessageChannel* peer_channel() override { return nullptr; }
};

struct Side {
    UdpMux mux; std::unique_ptr<MessageHub> hub; std::unique_ptr<FakeClient> cl; std::unique_ptr<RePsnRendezvous> rz; uint64_t clock = 1000; Path last = Path::None; int relay_calls = 0;
    std::vector<std::string> progress; std::string n1, n2;   // on_progress lines and the last notice
    void init(FakeServer& sv, uint32_t user, RePsnRendezvous::Config c) {
        std::string err; CHECK(mux.open(0, &err));
        Endpoint ep; char b[32]; std::snprintf(b, sizeof b, "127.0.0.1:%u", (unsigned)mux.local_port()); CHECK(parse_endpoint(b, ep));
        hub.reset(new MessageHub(mux)); cl.reset(new FakeClient(sv, user, ep));
        rz.reset(new RePsnRendezvous(*cl, mux, *hub, c));
        rz->now_ms = [this] { return clock; };
        rz->on_path = [this](Path p, uint32_t) { last = p; };
        rz->on_progress = [this](const std::string& l, const std::string& a, const std::string& b) { progress.push_back(l); n1 = a; n2 = b; };
        cl->clock = [this] { return clock; };
        rz->make_mesh_inner = [this](const RePsnRendezvous::MeshStart& s) { inner_members = s.members.size(); return new FakeInner(s); };
    }
    size_t inner_members = 0;
    // polls this side alone for `ms` of fake time in `step` ms steps; the result of the last poll (stops at a decision)
    int run_alone(uint64_t ms, uint64_t step = 50) { int r = 0; for (uint64_t e = 0; e < ms && r == 0; e += step) { clock += step; r = rz->poll(); } return r; }
};
// drives both sides until both are decided or `steps` run out; each step advances the fake clock 50 ms and sleeps 2 ms so the real loopback UDP can deliver
static void drive(Side& a, Side& b, int steps, int* ra, int* rb)
{
    *ra = *rb = 0;
    for (int i = 0; i < steps && (*ra == 0 || *rb == 0); i++) {
        a.clock += 50; b.clock += 50;
        a.mux.pump(); b.mux.pump(); a.hub->pump(); b.hub->pump();
        if (*ra == 0) *ra = a.rz->poll();
        if (*rb == 0) *rb = b.rz->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static RePsnRendezvous::Config cfg(bool host, uint32_t build, bool relay = false) { RePsnRendezvous::Config c; c.host = host; c.build = build; c.allow_relay = relay; return c; }
// ENHANCEMENT (four players): Alice hosts, Bob, Carol and Dan join (users 11..14); every side steps 50 ms of fake time per round until all decided or `rounds` run out
struct Mesh {
    FakeServer sv; Side s[4]; int r[4] = { 0, 0, 0, 0 }; int n;
    Mesh(uint32_t build, bool relay, bool offered, int count = 4, std::function<void(RePsnRendezvous::Config&)> tweak = nullptr) : n(count) {
        sv.relay = offered; const char* names[4] = { "Alice-rb", "Bob-rb", "Carol-rb", "Dan-rb" };
        for (int i = 0; i < n; i++) {
            auto c = cfg(i == 0, build, relay); c.players = 4; c.pair = 0x77; if (tweak) tweak(c);
            s[i].init(sv, 11 + (uint32_t)i, c); s[i].cl->name = names[i]; s[i].cl->allow = relay; sv.all.push_back(s[i].cl.get());
            s[i].cl->peers[0] = i ? s[0].cl.get() : nullptr;
        }
    }
    bool run(int rounds) {
        for (int k = 0; k < rounds; k++) {
            bool all = true;
            for (int i = 0; i < n; i++) { if (r[i]) continue; s[i].clock += 50; r[i] = s[i].rz->poll(); all = all && r[i]; }
            if (all) return true;
        }
        return false;
    }
};

int main()
{
    const uint32_t B = 0xA1B2C3D4u;
    // ---- record and room list ----
    CHECK(make_record(B) == "rb=1;ver=a1b2c3d4" && record_matches("rb=1;ver=a1b2c3d4", B) && !record_matches("rb=1;ver=a1b2c3d5", B) && !record_matches("rb=1;ver=a1b2c3d4;x=1", B) && !record_matches("", B));
    { std::vector<PsnrRoom> all(5);
      all[0] = { 7, kRoomFlagRollback, make_record(B), 1, 1, 2 };            // ok
      all[1] = { 3, kRoomFlagRollback, make_record(B), 1, 1, 2 };            // ok, older
      all[2] = { 4, 0, make_record(B), 1, 1, 2 };                            // no flag (a game room)
      all[3] = { 5, kRoomFlagRollback, make_record(B + 1), 1, 1, 2 };        // other build
      all[4] = { 6, kRoomFlagRollback, make_record(B), 1, 2, 2 };            // full
      const auto f = filter_rooms(all, B); CHECK(f.size() == 2 && f[0].id == 3 && f[1].id == 7); }
    { std::vector<PsnrMember> m = { { 50, 9, {}, "", "" }, { 60, 2, {}, "", "" }, { 70, 5, {}, "", "" } };
      CHECK(player_index(m, 60) == 0 && player_index(m, 70) == 1 && player_index(m, 50) == 2 && player_index(m, 99) == -1); }

    // ---- punch, then the plan: roles, session id = room id, player index, path "direct" ----
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B)); c.init(sv, 22, cfg(false, B)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      int rh, rc; drive(h, c, 400, &rh, &rc);
      CHECK(rh == 1 && rc == 1);
      CHECK(h.rz->path() == Path::Direct && c.rz->path() == Path::Direct && h.relay_calls == 0);
      CHECK(h.rz->plan().host && !c.rz->plan().host && h.rz->plan().session_id == h.rz->room_id() && c.rz->plan().session_id == h.rz->room_id());
      CHECK(h.rz->local_index() == 0 && c.rz->local_index() == 1);
      CHECK(h.cl->punches > 0 && c.cl->punches > 0);                         // PUNCH went out before the session started
      CHECK(sv.rooms.size() == 1 && (sv.rooms[0].r.flags & kRoomFlagRollback) && sv.rooms[0].r.external == make_record(B) && sv.rooms[0].members[0].data == make_record(B)); }

    // ---- version mismatch: never joined (room list); the joiner keeps looking, and its give-up names the build it saw ----
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B + 1)); c.init(sv, 22, cfg(false, B)); h.cl->peers[0] = c.cl.get();
      int rc = 0; for (int i = 0; i < 4; i++) { h.rz->poll(); rc = c.rz->poll(); }
      CHECK(rc == 0 && c.cl->joins == 0);
      rc = c.run_alone(130000);
      CHECK(rc == -1 && c.cl->joins == 0 && c.cl->leaves == 1);
      CHECK(!c.progress.empty() && c.progress.back().find(" room lists read; seen: a rollback room with build a1b2c3d5)") != std::string::npos);   // one level of parentheses
      CHECK(c.rz->error() == "re-psn: no host with this version found within 2 minutes (seen: a rollback room with build a1b2c3d5): start both games at about the same time "
                             "(the host first is best; each waits 2 minutes after its own start-up); both need the same version (build a1b2c3d4) and the same gameplay mods (or none)"); }

    // ---- the rendezvous window ----
    CHECK(kRendezvousWindowMs == 120000 && RePsnRendezvous::Config().give_up_ms == kRendezvousWindowMs && RePsnRendezvous::Config().lookup_every_ms == 2000);
    CHECK(window_words(120000) == "2 minutes" && window_words(60000) == "1 minute" && window_words(20000) == "20 seconds" && window_words(1000) == "1 second" && window_words(90000) == "90 seconds");
    { uint32_t b = 0; CHECK(record_build("rb=1;ver=0a1b2c3d", &b) && b == 0x0a1b2c3du && !record_build("rb=1;ver=0A1B2C3D", &b) && !record_build("rb=1;ver=0a1b2c3", &b) && !record_build("", &b)); }
    // the joiner starts first: it reads the room list every 2 s (never faster), and joins when the host's room appears 60 s later; they meet
    { FakeServer sv; Side h, c; c.init(sv, 22, cfg(false, B)); h.init(sv, 11, cfg(true, B)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      CHECK(c.run_alone(60100) == 0 && c.cl->joins == 0 && sv.rooms.empty());
      CHECK(c.cl->searches >= 29 && c.cl->searches <= 31 && c.rz->lookups == (uint64_t)c.cl->searches);
      bool spaced = true; for (size_t i = 1; i < c.cl->search_at.size(); i++) if (c.cl->search_at[i] - c.cl->search_at[i - 1] < 2000) spaced = false;
      CHECK(spaced);
      CHECK(c.progress[0].find("re-psn: looking for a host with build a1b2c3d4 (0 s of 120 s, ") == 0);   // already connected: never "connecting"
      CHECK(c.progress.size() == 7 && c.progress[6].find("re-psn: looking for a host with build a1b2c3d4 (60 s of 120 s, ") == 0 && c.progress[6].find("room lists read") != std::string::npos);
      CHECK(c.n1 == "Rollback: looking for the host's room (60 s of 120 s)." && c.n2 == "Both need the same version (build a1b2c3d4) and the same gameplay mods (or none).");
      int rh, rc; drive(h, c, 400, &rh, &rc);
      CHECK(rh == 1 && rc == 1 && c.cl->joins == 1 && h.rz->path() == Path::Direct && c.rz->plan().session_id == h.rz->room_id()); }
    // the host starts first and waits 100 s with its room listed the whole time; a joiner that starts then still meets it
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B)); c.init(sv, 22, cfg(false, B)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      CHECK(h.run_alone(100100) == 0 && sv.rooms.size() == 1 && h.cl->leaves == 0);
      CHECK(h.progress[0] == "re-psn: room open, waiting for a player with build a1b2c3d4 (0 s of 120 s)");
      CHECK(h.progress.size() == 11 && h.progress[10] == "re-psn: room open, waiting for a player with build a1b2c3d4 (100 s of 120 s)");
      CHECK(h.n1 == "Rollback: room open, waiting for the other player (100 s of 120 s).");
      int rh, rc; drive(h, c, 400, &rh, &rc);
      CHECK(rh == 1 && rc == 1 && c.cl->searches == 1); }
    // the host gives up at the window (not before), leaves its room, and says what to do
    { FakeServer sv; Side h; h.init(sv, 11, cfg(true, B));
      const uint64_t t0 = h.clock;
      CHECK(h.run_alone(119900) == 0 && h.cl->leaves == 0);
      const int r = h.run_alone(1000);
      CHECK(r == -1 && h.clock - t0 > 120000 && h.clock - t0 <= 120100 && h.cl->leaves == 1);
      CHECK(h.rz->error() == "re-psn: no player with this version joined within 2 minutes: start both games at about the same time (the host first is best; each waits 2 minutes after its own start-up); "
                             "both need the same version (build a1b2c3d4) and the same gameplay mods (or none)");
      CHECK(h.rz->notice1() == "Rollback could not start: no player with this version joined within 2 minutes.");
      CHECK(h.rz->notice2() == "Start both games at about the same time (the host first is best; each waits 2 minutes after its own start-up); both need the same version (build a1b2c3d4) and the same gameplay mods (or none).");
      CHECK(h.progress.size() == 13); }
    // the joiner gives up at the window when no host appears
    { FakeServer sv; Side c; c.init(sv, 22, cfg(false, B));
      const uint64_t t0 = c.clock;
      const int r = c.run_alone(125000);
      CHECK(r == -1 && c.clock - t0 > 120000 && c.clock - t0 <= 120100 && c.cl->joins == 0 && c.cl->searches >= 60 && c.cl->searches <= 61);
      CHECK(c.rz->notice1() == "Rollback could not start: no host with this version found within 2 minutes." && c.rz->error().find("(build a1b2c3d4)") != std::string::npos); }
    // a server that never answers HELLO: the window names that instead
    { FakeServer sv; Side c; c.init(sv, 22, cfg(false, B)); c.cl->is_ready = false;
      CHECK(c.run_alone(1000) == 0 && c.progress.size() == 1 && c.progress[0] == "re-psn: connecting to the server (0 s of 120 s, build a1b2c3d4)");
      CHECK(c.run_alone(125000) == -1 && c.rz->error().find("re-psn: the online server did not answer within 2 minutes: ") == 0 && c.cl->searches == 0); }
    // a shorter window from the config is used in the texts
    { FakeServer sv; Side c; auto cc = cfg(false, B); cc.give_up_ms = 20000; c.init(sv, 22, cc);
      CHECK(c.run_alone(30000) == -1 && c.rz->error().find("found within 20 seconds: start both games at about the same time (the host first is best; each waits 20 seconds after its own start-up)") != std::string::npos); }
    // a named room that is gone / another build: refused too
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B)); auto cc = cfg(false, B); cc.join_room = 999; c.init(sv, 22, cc);
      h.rz->poll(); int rc = c.rz->poll(); CHECK(rc == -1 && c.cl->joins == 0); }
    // ---- mismatch at join: the owner's member data differs (a client refuses the host) ----
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B)); c.init(sv, 22, cfg(false, B)); h.cl->peers[0] = c.cl.get();
      h.rz->poll(); sv.rooms[0].members[0].data = "rb=1;ver=00000000";     // the room list still says B, the member entry does not
      int rc = 0; for (int i = 0; i < 4; i++) rc = c.rz->poll();
      CHECK(rc == -1 && c.rz->error().find("does not match") != std::string::npos); }
    // the host refuses a joiner whose data differs and keeps waiting (counted), the session never starts
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B)); c.init(sv, 22, cfg(false, B)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      h.rz->poll(); c.rz->poll();
      for (auto& m : sv.rooms[0].members) if (m.user == 22) m.data = "rb=1;ver=deadbeef";
      for (auto& e : h.cl->ev) for (auto& m : e.members) if (m.user == 22) m.data = "rb=1;ver=deadbeef";
      int r = 0; for (int i = 0; i < 5; i++) { h.clock += 50; r = h.rz->poll(); }
      CHECK(r == 0 && h.rz->rejected_members == 1 && h.rz->path() == Path::None); }

    // ---- relay only when allowed AND offered: the direct path is dead (every datagram dropped), the relay is stood in for by lifting the drop ----
    auto dead = [](Side& s) { LinkModel lm; lm.loss_permille = 1000; s.mux.set_link_model(lm); };
    auto alive = [](Side& s) { s.mux.set_link_model(LinkModel()); };
    for (int variant = 0; variant < 4; variant++) {   // 0 not allowed (not offered), 1 allowed but the server offers none, 2 allowed and offered, 3 offered but NOT allowed
        const bool allow = variant == 1 || variant == 2;
        FakeServer sv; sv.relay = variant >= 2; Side h, c; h.init(sv, 11, cfg(true, B, allow)); c.init(sv, 22, cfg(false, B, allow));
        h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get(); dead(h); dead(c);
        h.rz->on_path = [&](Path p, uint32_t) { h.last = p; if (p == Path::Relay) { h.relay_calls++; alive(h); } };
        c.rz->on_path = [&](Path p, uint32_t) { c.last = p; if (p == Path::Relay) { c.relay_calls++; alive(c); } };
        int rh, rc; drive(h, c, 600, &rh, &rc);
        if (variant == 2) { CHECK(rh == 1 && rc == 1 && h.rz->path() == Path::Relay && c.rz->path() == Path::Relay && h.relay_calls >= 1 && c.relay_calls >= 1); }
        else {
            CHECK((rh == -1 || rc == -1) && h.relay_calls == 0 && c.relay_calls == 0);
            const std::string e = rh == -1 ? h.rz->error() : c.rz->error();
            CHECK(e.find(variant == 0 || variant == 3 ? "relay is not allowed" : "offers no relay") != std::string::npos);
        }
    }
    // a direct path never asks for the relay even when allowed and offered
    { FakeServer sv; sv.relay = true; Side h, c; h.init(sv, 11, cfg(true, B, true)); c.init(sv, 22, cfg(false, B, true)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      int rh, rc; drive(h, c, 400, &rh, &rc); CHECK(rh == 1 && rc == 1 && h.rz->path() == Path::Direct && c.rz->path() == Path::Direct); }
    // TEST ONLY force_relay: the relay at once, though the direct path works (one-PC relay tests); never without allow_relay
    for (int allow = 0; allow < 2; allow++) { FakeServer sv; sv.relay = true; auto ch = cfg(true, B, allow), cc = cfg(false, B, allow); ch.force_relay = cc.force_relay = true;
      Side h, c; h.init(sv, 11, ch); c.init(sv, 22, cc); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      h.rz->on_path = [&](Path p, uint32_t) { if (p == Path::Relay) h.relay_calls++; }; c.rz->on_path = [&](Path p, uint32_t) { if (p == Path::Relay) c.relay_calls++; };
      int rh, rc; drive(h, c, 400, &rh, &rc); CHECK(rh == 1 && rc == 1);
      if (allow) CHECK(h.rz->path() == Path::Relay && c.rz->path() == Path::Relay && h.relay_calls >= 1 && c.relay_calls >= 1);
      else CHECK(h.rz->path() == Path::Direct && c.rz->path() == Path::Direct && h.relay_calls == 0); }
    // a stranger that knows the host's UDP port sends a well-formed HELO first: the host's session is with the room's other player, never with the stranger
    { FakeServer sv; Side h, c, x; h.init(sv, 11, cfg(true, B)); c.init(sv, 22, cfg(false, B)); x.init(sv, 33, cfg(false, B)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      MessageChannel* ch = x.hub->open(h.cl->ep); const uint8_t hello[5] = { 'H', 'E', 'L', 'O', 1 };
      for (int i = 0; i < 40 && ch; i++) { ch->send(hello, 5); x.mux.pump(); x.hub->pump(); h.mux.pump(); h.hub->pump(); }   // the stranger first: its HELO is already queued at the host
      int rh, rc; drive(h, c, 600, &rh, &rc);
      CHECK(rh == 1 && rc == 1 && h.rz->plan().remotes.size() == 1 && h.rz->plan().remotes[0].ep == c.cl->ep); }
    // the other player leaving during the punch ends it
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B)); c.init(sv, 22, cfg(false, B)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get(); dead(h); dead(c);
      h.rz->poll(); c.rz->poll(); for (int i = 0; i < 6; i++) { h.clock += 50; h.rz->poll(); }
      PsnrEvent e; e.kind = PsnrEvent::MemberLeft; e.user = 22; h.cl->ev.push_back(e); int r = h.rz->poll(); CHECK(r == -1 && h.rz->error().find("left") != std::string::npos); }

    // ---- ENHANCEMENT (native-room pairing): the pair key (the game's own room id) in the record and the room filter ----
    { const uint64_t P = 0x00000001deadbeefull, Q = 0x00000001deadbef0ull;
      CHECK(make_record(B, 0) == make_record(B) && make_record(B, P) == "rb=1;ver=a1b2c3d4;pair=00000001deadbeef");
      CHECK(record_matches(make_record(B, P), B, P) && !record_matches(make_record(B, P), B, Q) && !record_matches(make_record(B, P), B) && !record_matches(make_record(B), B, P));
      uint32_t b = 0; uint64_t k = 7;
      CHECK(record_build(make_record(B, P), &b, &k) && b == B && k == P);
      CHECK(record_build(make_record(B), &b, &k) && b == B && k == 0);
      CHECK(!record_build("rb=1;ver=a1b2c3d4;pair=00000001DEADBEEF", &b) && !record_build("rb=1;ver=a1b2c3d4;pair=0000000000000000", &b) && !record_build("rb=1;ver=a1b2c3d4;pairs=0000001deadbeef", &b));
      std::vector<PsnrRoom> all(5);
      all[0] = { 9, kRoomFlagRollback, make_record(B, P), 1, 1, 2 };      // our native room's rollback room
      all[1] = { 4, kRoomFlagRollback, make_record(B, Q), 1, 1, 2 };      // another native room's (older)
      all[2] = { 5, kRoomFlagRollback, make_record(B), 1, 1, 2 };         // a plain (lobby / start-up) rollback room
      all[3] = { 6, kRoomFlagRollback, make_record(B + 1, P), 1, 1, 2 };  // our key, another build: refused
      all[4] = { 7, kRoomFlagRollback, make_record(B, P), 1, 2, 2 };      // our key, full
      auto f = filter_rooms(all, B, P); CHECK(f.size() == 1 && f[0].id == 9);
      f = filter_rooms(all, B, Q); CHECK(f.size() == 1 && f[0].id == 4);
      f = filter_rooms(all, B); CHECK(f.size() == 1 && f[0].id == 5);   // a plain joiner never takes a paired room
      CHECK(rollback_name("Player1") == "Player1-rb" && rollback_name("abcdefghijklm") == "abcdefghijklm-rb");
      const std::string l1 = rollback_name("abcdefghijklmnop"), l2 = rollback_name("abcdefghijklmnoq");
      CHECK(l1.size() == 16 && l2.size() == 16 && l1 != l2 && l1.compare(0, 12, "abcdefghijk-") == 0);
      CHECK(rollback_comm_id("NPWR08138_00") == "NPWR08138_RB"); }
    // two native rooms, each with its own pair key: every joiner meets the host of ITS room (the older room of the other key is skipped); the owner hosts (slot 0)
    { FakeServer sv; const uint64_t P = 0x1111, Q = 0x2222;
      auto pc = [&](bool host, uint64_t pair) { auto c = cfg(host, B); c.pair = pair; return c; };
      Side hq, hp, cp, cq; hq.init(sv, 11, pc(true, Q)); hp.init(sv, 12, pc(true, P)); cp.init(sv, 21, pc(false, P)); cq.init(sv, 22, pc(false, Q));
      cp.cl->peers[0] = hp.cl.get(); cp.cl->peers[1] = hq.cl.get(); cq.cl->peers[0] = hq.cl.get(); cq.cl->peers[1] = hp.cl.get();
      hq.rz->poll(); hp.rz->poll();   // Q's room is created first (the oldest): a joiner without the key filter would take it
      CHECK(sv.rooms.size() == 2 && sv.rooms[0].r.external == make_record(B, Q) && sv.rooms[1].r.external == make_record(B, P) && sv.rooms[1].members[0].data == make_record(B, P));
      int rh, rc; drive(hp, cp, 400, &rh, &rc);
      CHECK(rh == 1 && rc == 1 && cp.rz->room_id() == hp.rz->room_id() && cp.rz->plan().session_id == hp.rz->room_id() && cp.cl->joins == 1);
      CHECK(hp.rz->local_index() == 0 && cp.rz->local_index() == 1 && hp.rz->plan().host && !cp.rz->plan().host);
      drive(hq, cq, 400, &rh, &rc);
      CHECK(rh == 1 && rc == 1 && cq.rz->room_id() == hq.rz->room_id() && cq.rz->room_id() != hp.rz->room_id() && cq.cl->joins == 1); }
    // a joiner whose native room has no rollback room yet keeps looking (a room of another key is never taken) and gives up at the window
    { FakeServer sv; Side h, c; auto ch = cfg(true, B); ch.pair = 0x3333; auto cc = cfg(false, B); cc.pair = 0x4444; h.init(sv, 11, ch); c.init(sv, 22, cc); h.cl->peers[0] = c.cl.get();
      h.rz->poll();
      CHECK(c.run_alone(10000) == 0 && c.cl->joins == 0 && c.cl->searches >= 4);
      CHECK(c.run_alone(125000) == -1 && c.cl->joins == 0 && c.rz->error().find("no host with this version found") != std::string::npos); }
    // wrong pair key at join (the room list said ours, the owner's member data has another key): the joiner refuses; a host refuses a member with another key
    { FakeServer sv; Side h, c; auto ch = cfg(true, B); ch.pair = 0x5555; auto cc = ch; cc.host = false; h.init(sv, 11, ch); c.init(sv, 22, cc); h.cl->peers[0] = c.cl.get();
      h.rz->poll(); sv.rooms[0].members[0].data = make_record(B, 0x6666);
      int rc = 0; for (int i = 0; i < 4; i++) rc = c.rz->poll();
      CHECK(rc == -1 && c.rz->error().find("does not match") != std::string::npos); }
    { FakeServer sv; Side h, c; auto ch = cfg(true, B); ch.pair = 0x5555; auto cc = ch; cc.host = false; h.init(sv, 11, ch); c.init(sv, 22, cc);
      h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      h.rz->poll(); c.rz->poll();
      for (auto& e : h.cl->ev) for (auto& m : e.members) if (m.user == 22) m.data = make_record(B, 0x6666);
      int r = 0; for (int i = 0; i < 5; i++) { h.clock += 50; r = h.rz->poll(); }
      CHECK(r == 0 && h.rz->rejected_members == 1 && h.rz->path() == Path::None); }

    // ---- ENHANCEMENT (version mismatch message): the release table, the text, both sides fail at once (paired), an older host's give-up text ----
    { CHECK(release_name(0xe9be7f43u) == "0.7.0-test1" && release_name(0xf1e06d4du) == "0.7.0-test2" && release_name(0x8a47e98au) == "0.7.0-test3");
      CHECK(release_name(0x699bd950u) == "0.7.0-test4" && release_name(0x42959da6u) == "0.7.0-test5");
      CHECK(release_name(0x0000abcdu) == "build 0000abcd" && release_name(B) == "build a1b2c3d4");
      CHECK(version_mismatch_text(0xf1e06d4du, 0x8a47e98au, "") == "The other player is on 0.7.0-test2; you're on 0.7.0-test3. Both players need the same version for rollback.");
      CHECK(version_mismatch_text(0x8a47e98au, B, "0.7.0-test4") == "The other player is on 0.7.0-test3; you're on 0.7.0-test4. Both players need the same version for rollback.");
      CHECK(version_mismatch_text(0x12345678u, B, "0.7.0-test4") == "The other player is on build 12345678 (or uses different gameplay mods); you're on 0.7.0-test4. Both players need the same version for rollback.");
      // the same release name on both sides (other gameplay mods): the build words
      CHECK(version_mismatch_text(0x8a47e98au, B, "0.7.0-test3") == "The other player is on build 8a47e98a (or uses different gameplay mods); you're on build a1b2c3d4. Both players need the same version for rollback."); }
    // paired, the host on 0.7.0-test3 (B2) and a 0.7.0-test4 joiner (B): the joiner joins once, then fails; the host fails at that join, not at its window; nobody retries
    { FakeServer sv; const uint64_t P = 0x7777; const uint32_t B2 = 0x8a47e98au;
      auto ch = cfg(true, B2); ch.pair = P; auto cc = cfg(false, B); cc.pair = P; cc.release = "0.7.0-test4";
      Side h, c; h.init(sv, 11, ch); c.init(sv, 22, cc); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      CHECK(h.rz->poll() == 0 && sv.rooms.size() == 1 && sv.rooms[0].r.external == make_record(B2, P));   // the record format is unchanged
      int rc = 0; for (int i = 0; i < 4 && rc == 0; i++) { c.clock += 50; rc = c.rz->poll(); }
      CHECK(rc == -1 && c.cl->joins == 1 && c.rz->version_mismatch() && c.cl->leaves == 1);
      CHECK(c.rz->notice1() == "The other player is on 0.7.0-test3; you're on 0.7.0-test4. Both players need the same version for rollback." && !c.rz->notice2().empty());
      CHECK(c.rz->error() == "re-psn: the other player runs another version (build 8a47e98a, yours a1b2c3d4)");
      h.clock += 50; const int rh = h.rz->poll();   // the host's release.txt is "" here (a local build): its own side by the table
      CHECK(rh == -1 && h.rz->version_mismatch() && h.rz->notice1() == "The other player is on build a1b2c3d4 (or uses different gameplay mods); you're on 0.7.0-test3. Both players need the same version for rollback.");
      CHECK(h.clock < 2000); }
    // the joiner's join and leave reach the host in one poll: the host still fails with the message (checked at MemberJoined)
    { FakeServer sv; const uint64_t P = 0x7778; const uint32_t B2 = 0xf1e06d4du;
      auto ch = cfg(true, B); ch.pair = P; ch.release = "0.7.0-test3"; Side h; h.init(sv, 11, ch);
      CHECK(h.rz->poll() == 0);
      PsnrEvent j; j.kind = PsnrEvent::MemberJoined; j.members = { PsnrMember{ 22, 9, Endpoint{}, make_record(B2, P), std::string() } }; h.cl->ev.push_back(j);
      PsnrEvent l; l.kind = PsnrEvent::MemberLeft; l.user = 22; h.cl->ev.push_back(l);
      CHECK(h.rz->poll() == -1 && h.rz->version_mismatch() && h.rz->notice1() == "The other player is on 0.7.0-test2; you're on 0.7.0-test3. Both players need the same version for rollback."); }
    // a member of another native room (another pair key) is not "another version": refused as before, no mismatch
    { FakeServer sv; auto ch = cfg(true, B); ch.pair = 0x7779; Side h; h.init(sv, 11, ch); h.rz->poll();
      PsnrEvent j; j.kind = PsnrEvent::MemberJoined; j.members = { PsnrMember{ 22, 9, Endpoint{}, make_record(B + 1, 0x777a), std::string() } }; h.cl->ev.push_back(j);
      CHECK(h.rz->poll() == 0 && !h.rz->version_mismatch() && h.rz->rejected_members == 1); }
    // the server does not answer the joiner's one join: it fails with the message after kMismatchJoinMs
    { FakeServer sv; const uint64_t P = 0x777b; auto ch = cfg(true, B + 1); ch.pair = P; auto cc = cfg(false, B); cc.pair = P;
      Side h, c; h.init(sv, 11, ch); c.init(sv, 22, cc); h.rz->poll();
      CHECK(c.rz->poll() == 0 && c.cl->joins == 1); c.cl->ev.clear();
      CHECK(c.run_alone(kMismatchJoinMs - 100) == 0);
      CHECK(c.run_alone(300) == -1 && c.rz->version_mismatch()); }
    // a paired host whose joiner never comes (a joiner before 0.7.0-test4 on another version only says so on its own screen): the 120 s give-up says why
    { FakeServer sv; auto ch = cfg(true, B); ch.pair = 0x777c; Side h; h.init(sv, 11, ch);
      CHECK(h.run_alone(119000) == 0 && h.run_alone(2000) == -1 && !h.rz->version_mismatch());
      CHECK(h.rz->notice1() == "Rollback could not start: the other player probably runs a different version (the main menu shows it), or their online connection failed.");
      CHECK(h.rz->error().rfind("re-psn: the other player probably runs a different version (the main menu shows it), or their online connection failed: start both games", 0) == 0); }
    // ---- ENHANCEMENT (four players): the mesh with four members ----
    CHECK(player_label("Bob-rb") == "Bob" && player_label("abcdefghijk-1a2b") == "abcdefghijk..." && player_label("") == "a player" && player_label("Carol") == "Carol");
    CHECK(name_list({ "A" }) == "A" && name_list({ "A", "B" }) == "A and B" && name_list({ "A", "B", "C" }) == "A, B and C");
    // all direct: room of 4, every PC 3 direct links, one session id, the owner hosts, player index by member id, the inner layer gets all 4 members
    { Mesh m(B, false, false); CHECK(m.run(400));
      CHECK(m.sv.rooms.size() == 1 && m.sv.rooms[0].r.max_members == 4 && m.sv.rooms[0].members.size() == 4);
      for (int i = 0; i < 4; i++) {
          CHECK(m.r[i] == 1 && m.s[i].rz->links().size() == 3 && m.s[i].rz->path() == Path::Direct && m.s[i].rz->path_line() == "Rollback on: direct connection");
          CHECK(m.s[i].rz->plan().session_id == m.s[0].rz->room_id() && m.s[i].rz->plan().host == (i == 0) && m.s[i].rz->local_index() == i && m.s[i].inner_members == 4);
          for (const auto& l : m.s[i].rz->links()) CHECK(l.path == Path::Direct && !l.name.empty() && l.name.find("-rb") == std::string::npos);
      }
      CHECK(m.s[0].clock - 1000 < 3000); }   // no wait for a punch window when every PUNCH is heard
    // one pair (Carol, Dan) cannot punch: that link goes through the server after the punch window, every other link stays direct
    { Mesh m(B, true, true); m.sv.cut = { { 13, 14 }, { 14, 13 } }; CHECK(m.run(400));
      for (int i = 0; i < 4; i++) CHECK(m.r[i] == 1);
      CHECK(m.s[0].rz->path_line() == "Rollback on: direct connection" && m.s[1].rz->path_line() == "Rollback on: direct connection");
      CHECK(m.s[2].rz->path_line() == "Rollback on: 1 of 3 links through the server" && m.s[3].rz->path_line() == "Rollback on: 1 of 3 links through the server");
      for (const auto& l : m.s[2].rz->links()) CHECK(l.path == (l.user == 14 ? Path::Relay : Path::Direct));
      CHECK(m.s[2].cl->links[14].relay && m.s[3].cl->links[13].relay && !m.s[2].cl->links[11].relay); }
    // one way only (Dan never hears Carol, Carol hears Dan): Dan relays after the window and Carol follows at once (never a half-direct link)
    { Mesh m(B, true, true); m.sv.cut = { { 13, 14 } }; CHECK(m.run(400));
      for (int i = 0; i < 4; i++) CHECK(m.r[i] == 1);
      CHECK(m.s[2].rz->path_line() == "Rollback on: 1 of 3 links through the server" && m.s[3].rz->path_line() == "Rollback on: 1 of 3 links through the server"); }
    // Dan cannot be reached by anyone and the relay is not allowed: everyone stops at the link bound (10 s), naming who could not connect
    { Mesh m(B, false, true); m.sv.cut = { { 11, 14 }, { 12, 14 }, { 13, 14 }, { 14, 11 }, { 14, 12 }, { 14, 13 } }; CHECK(m.run(400));
      for (int i = 0; i < 4; i++) CHECK(m.r[i] == -1);
      CHECK(m.s[0].rz->error() == "re-psn: could not connect to Dan within 10 seconds (relay is not allowed in the settings)");
      CHECK(m.s[0].rz->notice1() == "Rollback could not start: could not connect to Dan." && m.s[0].rz->notice2().find("through the server") != std::string::npos);
      CHECK(m.s[3].rz->error().find("could not connect to Alice, Bob and Carol within 10 seconds") != std::string::npos);
      CHECK(m.s[0].clock - 1000 <= 10000 + 1500 && m.s[0].clock - 1000 > 10000); }
    // the same with the relay allowed but no relay on the server
    { Mesh m(B, true, false); m.sv.cut = { { 11, 14 }, { 14, 11 } }; CHECK(m.run(400));
      CHECK(m.r[0] == -1 && m.s[0].rz->error() == "re-psn: could not connect to Dan within 10 seconds (this server offers no relay)"); }
    // Dan never starts: the room waits the window and then names him (the expected names come from the game's own room)
    { Mesh m(B, false, false, 3, [](RePsnRendezvous::Config& c) { c.expect_names = { "Alice-rb", "Bob-rb", "Carol-rb", "Dan-rb" }; c.give_up_ms = 20000; });
      CHECK(m.run(1000));
      for (int i = 0; i < 3; i++) CHECK(m.r[i] == -1 && m.s[i].rz->error().find("only 3 of 4 players (missing: Dan) in the room within 20 seconds") != std::string::npos);
      CHECK(!m.s[0].progress.empty() && m.s[0].progress.back().find("3 of 4 players (missing: Dan)") != std::string::npos); }
    // mixed builds (4-player online: the release's version mismatch rule applies to the mesh too): Dan's record says another build of our native room: no mesh can run,
    // so every PC fails at once with the version message (no retry), as two players do
    { Mesh m(B, false, false);
      m.s[0].rz->poll(); for (int i = 1; i < 4; i++) m.s[i].rz->poll();
      for (auto& x : m.sv.rooms[0].members) if (x.user == 14) x.data = make_record(B + 1, 0x77, 4);
      for (auto& c : m.sv.all) for (auto& e : c->ev) for (auto& x : e.members) if (x.user == 14) x.data = make_record(B + 1, 0x77, 4);
      m.run(40);
      for (int i = 0; i < 3; i++) CHECK(m.r[i] == -1 && m.s[i].rz->version_mismatch() && m.s[i].rz->notice1().find("Both players need the same version") != std::string::npos);
      CHECK(m.s[0].rz->error() == "re-psn: the other player runs another version (build a1b2c3d5, yours a1b2c3d4)"); }
    // a member of another native room (same build, another pair key) got in: the host refuses and kicks it by name, Bob and Carol do not fail, the room waits
    { Mesh m(B, false, false);
      m.s[0].rz->poll(); for (int i = 1; i < 4; i++) m.s[i].rz->poll();
      for (auto& x : m.sv.rooms[0].members) if (x.user == 14) x.data = make_record(B, 0x78, 4);
      for (auto& c : m.sv.all) for (auto& e : c->ev) for (auto& x : e.members) if (x.user == 14) x.data = make_record(B, 0x78, 4);
      m.run(40);
      CHECK(m.r[0] == 0 && m.r[1] == 0 && m.r[2] == 0 && m.s[0].rz->rejected_members == 1 && m.s[0].cl->kicks == 1 && m.sv.rooms[0].members.size() == 3 && !m.s[0].rz->version_mismatch());
      m.s[0].clock += 20000; m.run(1);
      CHECK(!m.s[0].progress.empty() && m.s[0].progress.back().find("3 of 4 players (refused, another version: Dan (another room))") != std::string::npos); }
    // a 3-player record in a 4-player mesh (same build and pair: a stale plan) is not "another version": refused like another room
    { Mesh m(B, false, false);
      m.s[0].rz->poll(); for (int i = 1; i < 4; i++) m.s[i].rz->poll();
      for (auto& x : m.sv.rooms[0].members) if (x.user == 14) x.data = make_record(B, 0x77, 3);
      for (auto& c : m.sv.all) for (auto& e : c->ev) for (auto& x : e.members) if (x.user == 14) x.data = make_record(B, 0x77, 3);
      m.run(40);
      CHECK(m.r[0] == 0 && m.s[0].rz->rejected_members == 1 && !m.s[0].rz->version_mismatch()); }
    // a client refuses a host whose record differs (per member, as with two players): another room's record (same build) = "does not match"
    { Mesh m(B, false, false);
      m.s[0].rz->poll(); m.sv.rooms[0].members[0].data = make_record(B, 0x78, 4); m.run(40);
      CHECK(m.r[1] == -1 && m.s[1].rz->error().find("does not match") != std::string::npos && !m.s[1].rz->version_mismatch()); }
    // ENHANCEMENT (4-player online): the host of our native room on another build: the client fails with the version message (the release's rule, now in the mesh too)
    { Mesh m(B, false, false);
      m.s[0].rz->poll(); m.sv.rooms[0].members[0].data = make_record(B + 1, 0x77, 4); m.run(40);
      CHECK(m.r[1] == -1 && m.s[1].rz->version_mismatch()); }
    // a member that leaves while its link is still down ends it, naming it; without the N-peer layer the mesh refuses to start
    { Mesh m(B, false, false); m.sv.cut = { { 11, 14 }, { 14, 11 } }; m.run(40);
      PsnrEvent e; e.kind = PsnrEvent::MemberLeft; e.user = 14; m.s[0].cl->ev.push_back(e);
      CHECK(m.s[0].rz->poll() == -1 && m.s[0].rz->error() == "re-psn: could not connect to Dan (relay is not allowed in the settings; Dan gave up)"); }
    { Mesh m(B, false, false); for (auto& x : m.s) x.rz->make_mesh_inner = nullptr; m.run(100);
      CHECK(m.r[0] == -1 && m.s[0].rz->error() == "re-psn: the 4-player session layer is not available in this build"); }
    // two players: the room max stays 2 and the "Rollback on" lines are today's
    { FakeServer sv; Side h, c; h.init(sv, 11, cfg(true, B)); c.init(sv, 22, cfg(false, B)); h.cl->peers[0] = c.cl.get(); c.cl->peers[0] = h.cl.get();
      int rh, rc; drive(h, c, 400, &rh, &rc);
      CHECK(rh == 1 && sv.rooms[0].r.max_members == 2 && h.rz->links().empty() && h.rz->path_line() == "Rollback on: direct connection" && h.inner_members == 0); }

    // ---- the notice store (jsp_net_game.cpp): every end of the wait leaves it right ----
    { RendezvousNotice n; std::string a, b; const unsigned g0 = n.gen();
      CHECK(!n.read(a, b));
      n.waiting("Rollback: room open, waiting for the other player (40 s of 120 s).", "Both need ...");
      CHECK(n.read(a, b) && a.find("waiting") != std::string::npos && n.gen() == g0 + 1);
      n.waiting("Rollback: room open, waiting for the other player (40 s of 120 s).", "Both need ..."); CHECK(n.gen() == g0 + 1);   // the same text: no redraw
      n.ended();   // another part ended the session before the games met (a mods refusal during the wait): the waiting band must go
      CHECK(!n.read(a, b) && n.gen() == g0 + 2);
      n.waiting("w", "x"); n.failed("Rollback could not start: x.", "Start ...");
      CHECK(n.read(a, b) && a == "Rollback could not start: x." && b == "Start ..." && n.gen() == g0 + 4);
      n.waiting("w", "x"); n.met(); CHECK(!n.read(a, b) && n.gen() == g0 + 6); }

    if (g_fail) { std::printf("%d of %d checks FAILED\n", g_fail, g_n); return 1; }
    std::printf("net repsn: all %d checks passed\n", g_n);
    return 0;
}
