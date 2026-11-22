// Host test of the post-game / pick wiring (installer/port/jsp/jsp_net_pick_flow.h): the 3..4 player choice (wire, states, timeouts, random
// loss / reordering / duplication), the 2-player path through the same interface, the message routing of the link's 'P' / 'C' tags, and the
// whole state flow of N = 2..4 PCs (prompt -> Fight again / Select character -> pick round -> the host's start -> the roster write at the
// builder), checking that every PC applies the same roster bytes. FIX (pick7): the switch handshake (the prompt opens only when every PC has
// JSP_ROLLBACK_PICK=1; with one PC on, no prompt and no stall on either). No game, no GPU.
#include "../port/jsp/jsp_net_pick_flow.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace jspf;

// ---- 1. the group message ----
static void test_group_wire()
{
    for (int pk = 0; pk < 4; pk++)
        for (int pl = 0; pl < 4; pl++) {
            GroupMsg m; m.session = 0xA1B2C3D4u; m.seq = 0x1234; m.rev = 0xBEEF; m.pick = (jspg::Choice)pk; m.player = (uint8_t)pl; m.saw = (uint8_t)(0xF >> pl);
            uint8_t b[kGroupMsgLen]; group_encode(m, b);
            GroupMsg d; CHECK(group_decode(b, kGroupMsgLen, &d));
            CHECK(d.seq == m.seq && d.rev == m.rev && d.pick == m.pick && d.player == m.player && d.saw == m.saw);
            for (size_t n = 0; n < kGroupMsgLen; n++) CHECK(!group_decode(b, n, &d));
            for (size_t i = 0; i < kGroupMsgLen; i++)
                for (int bit = 0; bit < 8; bit++) { uint8_t c[kGroupMsgLen]; std::memcpy(c, b, kGroupMsgLen); c[i] ^= (uint8_t)(1 << bit); CHECK(!group_decode(c, kGroupMsgLen, &d)); }
        }
    std::mt19937 r(9); int passed = 0; GroupMsg d;
    for (int i = 0; i < 20000; i++) { uint8_t g[kGroupMsgLen]; for (auto& x : g) x = (uint8_t)r(); if (group_decode(g, kGroupMsgLen, &d)) passed++; }
    CHECK(passed == 0);
    // the 2-player message never decodes as a group one and the other way round (lengths differ)
    jspg::Msg two; uint8_t t[jspg::kMsgLen]; jspg::encode(two, t);
    CHECK(!group_decode(t, jspg::kMsgLen, &d));
    GroupMsg g; g.session = 9; uint8_t gb[kGroupMsgLen]; group_encode(g, gb); jspg::Msg tm; CHECK(!jspg::decode(gb, kGroupMsgLen, &tm));
    GroupMsg z; uint8_t zb[kGroupMsgLen]; group_encode(z, zb); CHECK(!group_decode(zb, kGroupMsgLen, &d));   // session tag 0
}

// ---- 2. group states (3 players) ----
static const uint32_t kS = 0x5151u;   // the link's session tag
static void deliver(GroupAgreement& from, GroupAgreement& to, uint64_t now) { GroupMsg m; if (from.take(now, &m)) to.receive(m, now); }
static void test_group_states()
{
    GroupAgreement a, b, c;
    CHECK(!a.start(kS, 1, 0, 5, 0) && !a.start(kS, 1, 3, 3, 0));
    CHECK(a.start(kS, 4, 0, 3, 0) && b.start(kS, 4, 1, 3, 0) && c.start(kS, 4, 2, 3, 0));
    CHECK(a.pick(jspg::REMATCH, 10) && !a.pick(jspg::CHARSELECT, 11) && a.pick(jspg::REMATCH, 12));   // final at once
    CHECK(b.pick(jspg::REMATCH, 10) && c.pick(jspg::CHARSELECT, 10));
    for (uint64_t t = 20; t < 3000; t += 50) {
        GroupAgreement* p[3] = { &a, &b, &c };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (i != j) { GroupMsg m = p[i]->current(); p[j]->receive(m, t); }
    }
    CHECK(a.outcome() == jspg::O_CHARSELECT && b.outcome() == jspg::O_CHARSELECT && c.outcome() == jspg::O_CHARSELECT);
    // a LEAVE decides at once for whoever hears it
    CHECK(a.start(kS, 5, 0, 3, 0) && b.start(kS, 5, 1, 3, 0) && c.start(kS, 5, 2, 3, 0));
    CHECK(b.pick(jspg::LEAVE, 5) && b.outcome() == jspg::O_LEAVE && b.reason() == jspg::R_SELF);
    deliver(b, a, 6); CHECK(a.outcome() == jspg::O_LEAVE && a.reason() == jspg::R_PEER_LEFT);
    CHECK(std::strcmp(GroupAgreement::outcome_text(a.outcome(), a.reason()), "A player left: back to the room") == 0);
    // silence of one player
    jspg::Config cf; cf.idle_ms = 60000;
    CHECK(a.start(kS, 6, 0, 3, 0, cf) && c.start(kS, 6, 2, 3, 0, cf));
    for (uint64_t t = 0; t < 7900; t += 100) { a.receive(c.current(), t); a.tick(t); }
    CHECK(a.outcome() == jspg::PENDING);
    a.tick(8000); CHECK(a.outcome() == jspg::O_LEAVE && a.reason() == jspg::R_LINK_LOST);
    // idle: no choice in time
    cf.idle_ms = 10000;
    CHECK(a.start(kS, 7, 0, 3, 0, cf) && b.start(kS, 7, 1, 3, 0, cf) && c.start(kS, 7, 2, 3, 0, cf));
    for (uint64_t t = 0; t < 10000; t += 100) { a.receive(b.current(), t); a.receive(c.current(), t); a.tick(t); }
    CHECK(a.outcome() == jspg::PENDING && a.view(0, 9000).seconds_left == 1);
    a.receive(b.current(), 10000); a.receive(c.current(), 10000); a.tick(10000);
    CHECK(a.outcome() == jspg::O_LEAVE && a.reason() == jspg::R_IDLE && a.mine() == jspg::LEAVE);
    // a message of the next post-game is kept aside and taken at its start
    GroupAgreement x, y;
    CHECK(x.start(kS, 8, 0, 3, 0) && y.start(kS, 9, 1, 3, 0) && y.pick(jspg::REMATCH, 1));
    GroupMsg early = y.current(); CHECK(!x.receive(early, 2));
    CHECK(x.start(kS, 9, 0, 3, 3) && x.choice_of(1) == jspg::REMATCH);
    CHECK(x.view(0, 3).peer_line == "1 of 2 other players have chosen");
    // a message of another link (session tag) never counts, kept aside or not
    GroupAgreement o; CHECK(o.start(kS + 1, 9, 2, 3, 0) && o.pick(jspg::CHARSELECT, 1));
    CHECK(!x.receive(o.current(), 4) && x.choice_of(2) == jspg::NONE);
    CHECK(x.start(kS, 10, 0, 3, 5) && x.choice_of(2) == jspg::NONE);
}

// random reordered / lost / duplicated traffic between n group agreements: everyone who decides decides the same
static bool group_random(uint32_t seed, int n)
{
    std::mt19937 r(seed);
    GroupAgreement g[4];
    jspg::Config cf; cf.idle_ms = 3600000;
    for (int i = 0; i < n; i++) g[i].start(kS, 3, i, n, 0, cf);
    jspg::Choice want[4]; uint64_t at[4];
    for (int i = 0; i < n; i++) { want[i] = r() % 3 == 0 ? jspg::CHARSELECT : jspg::REMATCH; at[i] = r() % 2000; }
    struct Pkt { uint64_t t; int to; GroupMsg m; };
    std::vector<Pkt> q;
    for (uint64_t t = 0; t < 30000; t += 10) {
        for (int i = 0; i < n; i++) if (t >= at[i] && g[i].mine() == jspg::NONE) g[i].pick(want[i], t);
        for (int i = 0; i < n; i++) { g[i].tick(t); GroupMsg m; if (g[i].take(t, &m)) for (int j = 0; j < n; j++) if (j != i) { if (r() % 4 == 0) continue; q.push_back({ t + r() % 300, j, m }); if (r() % 6 == 0) q.push_back({ t + r() % 600, j, m }); } }
        std::vector<Pkt> rest;
        for (const Pkt& p : q) { if (p.t <= t) g[p.to].receive(p.m, t); else rest.push_back(p); }
        q.swap(rest);
        bool all = true; for (int i = 0; i < n; i++) if (g[i].outcome() == jspg::PENDING) all = false;
        if (all) break;
    }
    jspg::Outcome exp = jspg::O_REMATCH; for (int i = 0; i < n; i++) if (want[i] == jspg::CHARSELECT) exp = jspg::O_CHARSELECT;
    for (int i = 0; i < n; i++) if (g[i].outcome() != exp) return false;
    return true;
}

// ---- 3. Postgame with 2 players is jspg::Agreement ----
static void test_postgame_two()
{
    Postgame a, b; a.configure(0, 2); b.configure(1, 2);
    CHECK(a.start(2, 0) && b.start(2, 0));
    CHECK(a.pick(jspg::REMATCH, 1) && b.pick(jspg::REMATCH, 1));
    uint8_t buf[Postgame::kMaxMsg];
    for (uint64_t t = 2; t < 2000; t += 100) {
        size_t n = a.take(t, buf); if (n) { CHECK(n == jspg::kMsgLen); b.receive_bytes(buf, n, t); }
        n = b.take(t, buf); if (n) a.receive_bytes(buf, n, t);
    }
    CHECK(a.outcome() == jspg::O_REMATCH && b.outcome() == jspg::O_REMATCH);
    CHECK(std::string(a.outcome_text()) == "Rematch: same characters and stage");
}

// ---- 4. the flow of N PCs ----
struct Net {
    struct Pkt { uint64_t t; int to, from; std::vector<uint8_t> d; };
    std::vector<Pkt> q; std::mt19937 r; int loss = 5, dup = 8; uint32_t maxd = 200;
    explicit Net(uint32_t s) : r(s) {}
    void send(int from, int n, const uint8_t* b, size_t len, uint64_t t)
    {
        for (int j = 0; j < n; j++) {
            if (j == from || (loss && r() % (uint32_t)loss == 0)) continue;
            q.push_back({ t + r() % maxd, j, from, std::vector<uint8_t>(b, b + len) });
            if (dup && r() % (uint32_t)dup == 0) q.push_back({ t + r() % (2 * maxd), j, from, std::vector<uint8_t>(b, b + len) });
        }
    }
    template <class F> void deliver(uint64_t t, F&& f)
    {
        std::vector<Pkt> rest, now;
        for (Pkt& p : q) (p.t <= t ? now : rest).push_back(std::move(p));
        q.swap(rest);
        std::shuffle(now.begin(), now.end(), r);
        for (const Pkt& p : now) f(p.to, p.from, p.d.data(), p.d.size());
    }
};

// A room image as the host's broadcast leaves it on every PC: players' records at their battle slots (member ids), COMs elsewhere.
static void make_room(uint8_t img[jspk::kRoomSize], int n, const uint8_t* slots, const uint16_t* members, uint32_t seed)
{
    std::mt19937 r(seed);
    for (uint32_t i = 0; i < jspk::kRoomSize; i++) img[i] = (uint8_t)r();
    jspk::put_be32(img + jspk::kOffTeamSupport0, 0xC9); jspk::put_be32(img + jspk::kOffTeamSupport1, 0xCA);
    for (int i = 0; i < 4; i++) img[jspk::kOffSlotTeam + i] = 2;
    int t[4] = { 0, 0, 1, 1 };
    for (int rcd = 0; rcd < 4; rcd++) {
        uint8_t* p = img + jspk::kOffRecords + jspk::kRecSize * rcd;
        jspk::put_be16(p + jspk::kRecMember, 0); p[jspk::kRecCom] = 1; p[jspk::kRecTeam] = (uint8_t)t[rcd];
        jspk::put_be16(p + jspk::kRecFighter, (uint16_t)(0x10 + rcd)); jspk::put_be16(p + jspk::kRecSupport, (uint16_t)(t[rcd] ? 0xCA : 0xC9));
    }
    for (int i = 0; i < n; i++) {
        uint8_t* p = img + jspk::kOffRecords + jspk::kRecSize * slots[i];
        jspk::put_be16(p + jspk::kRecMember, members[i]); p[jspk::kRecCom] = 0;
    }
}

struct Pc { Flow f; bool chose = false, over = false; uint64_t over_at = 0, choose_at = 0, pick_at = 0, room_at = 0; jspg::Choice want = jspg::NONE; jspk::Pick pick; Act last = Act::None; bool ready = false; };

// One post-game on N PCs. force: every PC's choice (NONE = random). Returns false on a failed check (printed by the caller).
static bool flow_round(std::mt19937& r, Net& net, Pc* pc, int n, uint16_t seq, jspg::Choice force, const uint8_t* slots, const uint16_t* members, uint64_t* clock, bool* had_pair, bool verbose)
{
    uint64_t t = *clock;
    for (int i = 0; i < n; i++) {
        Pc& p = pc[i]; p.chose = false; p.over = false; p.over_at = t + r() % 500; p.ready = false; p.last = Act::None;
        p.want = force != jspg::NONE ? force : (r() % 3 == 0 ? jspg::CHARSELECT : jspg::REMATCH);
        p.choose_at = p.over_at + 100 + r() % 3000; p.pick_at = 0; p.room_at = 0;
        p.pick.fighter = (uint16_t)(r() % 3 == 0 ? jspk::kRandom : 0x20 + r() % 40); p.pick.support = (uint16_t)(r() % 2 ? jspk::kRandom : 0xC8 + r() % 14);
        p.pick.mode = r() % 2 ? jspk::TOGETHER : jspk::VERSUS;
    }
    bool any_cs = false; for (int i = 0; i < n; i++) any_cs |= pc[i].want == jspg::CHARSELECT;
    const Act expect = any_cs ? Act::CharSelect : Act::Rematch;
    bool started = false; PairOut at_start[4]; bool have[4] = {};
    for (int step = 0; step < 6000 && !started; step++, t += 10) {
        net.deliver(t, [&](int to, int from, const uint8_t* d, size_t len) { if (!pc[to].f.route(from, d, len, t)) { std::printf("FAIL: a tagged message was not routed\n"); fails++; } });
        if (t % 100 == 0) for (int i = 0; i < n; i++) pc[i].f.pump(t, [&](const uint8_t* b, size_t len) { net.send(i, n, b, len, t); });
        for (int i = 0; i < n; i++) {
            Pc& p = pc[i];
            if (!p.over) {   // the PCs end the match at different times; the switch handshake agreed before the first match (flow_random)
                if (t < p.over_at) continue;
                p.over = true;
                if (!p.f.match_end(seq, t)) { std::printf("FAIL: PC %d declined match %u (why %d)\n", i, seq, p.f.decline_why()); return false; }
            }
            if (!p.chose && t >= p.choose_at) { p.chose = true; if (!p.f.choose(p.want, t)) { std::printf("FAIL: choose refused\n"); return false; } }
            if (!p.f.hold() && p.last == Act::None && p.f.postgame().outcome() == jspg::PENDING) { std::printf("FAIL: no hold while the prompt is open\n"); return false; }
            const Act a = p.f.poll(t);
            if (a == Act::Rematch || a == Act::CharSelect) {
                if (a != expect) { std::printf("FAIL: PC %d act %s, expected %s\n", i, act_name(a), act_name(expect)); return false; }
                p.last = a; p.room_at = t + 200 + r() % 4000; if (a == Act::CharSelect) p.pick_at = t + 300 + r() % 5000;
                if (!p.f.hold()) { std::printf("FAIL: no hold during the pick round\n"); return false; }
            } else if (a == Act::Ready) p.ready = true;
            else if (a != Act::None) { std::printf("FAIL: PC %d act %s\n", i, act_name(a)); return false; }
            if (p.room_at && t >= p.room_at) { p.f.in_room(true); p.room_at = 0; }
            if (p.pick_at && t >= p.pick_at) { if (!p.f.submit(p.pick, true)) { std::printf("FAIL: submit refused\n"); return false; } p.pick_at = 0; }
        }
        if (pc[0].f.start_ok()) {   // the host presses "Go to battle": from here every PC must already have committed the round
            started = true;
            for (int i = 0; i < n; i++) if (pc[i].f.round().state() != jspk::READY) { std::printf("FAIL: the host may start while PC %d is not READY\n", i); return false; }
            for (int i = 0; i < n; i++) have[i] = pc[i].f.pair(&at_start[i]);
        }
    }
    *clock = t;
    if (!started) { std::printf("FAIL: the host never got to start (n=%d)\n", n); return false; }
    const bool want_pair = expect == Act::CharSelect || *had_pair;
    for (int i = 0; i < n; i++) if (have[i] != want_pair) { std::printf("FAIL: PC %d pair %d, expected %d\n", i, (int)have[i], (int)want_pair); return false; }
    if (want_pair) {
        uint8_t base[jspk::kRoomSize]; make_room(base, n, slots, members, (uint32_t)r());
        uint8_t out[4][jspk::kRoomSize];
        for (int i = 0; i < n; i++) {
            std::memcpy(out[i], base, sizeof base);
            MemberMap how; const jspk::Result res = apply_pair(out[i], at_start[i], nullptr, &how);
            if (res != jspk::Result::Ok || how != MemberMap::Slot) { std::printf("FAIL: apply %s %s\n", jspk::result_text(res), member_map_text(how)); return false; }
        }
        for (int i = 1; i < n; i++) {   // every byte is the same everywhere (the own pick block R+0x08 / +0x0C is not written)
            for (uint32_t b = 0; b < jspk::kRoomSize; b++) {
                if (out[i][b] != out[0][b]) { std::printf("FAIL: PC %d roster byte %03X differs\n", i, b); return false; }
            }
        }
        for (int i = 0; i < n; i++) {   // each PC's own record carries its own pick (unless Random)
            const jspk::Pick pk = at_start[i].pick[i];
            const uint8_t* rec = out[i] + jspk::kOffRecords + jspk::kRecSize * slots[i];
            if (pk.fighter != jspk::kRandom && jspk::be16(rec + jspk::kRecFighter) != pk.fighter) { std::printf("FAIL: PC %d fighter\n", i); return false; }
            if (std::memcmp(out[i] + jspk::kOffSelfFighter, base + jspk::kOffSelfFighter, 8)) { std::printf("FAIL: PC %d own pick block written\n", i); return false; }
            if (at_start[i].mode() != pc[0].pick.mode && expect == Act::CharSelect) { std::printf("FAIL: mode is not the host's\n"); return false; }
        }
        *had_pair = true;
    }
    // the fight runs everywhere: the round stops, the pair stays for the next Fight again
    for (int i = 0; i < n; i++) { pc[i].f.fight_running(); PairOut p; if (pc[i].f.pair(&p) != *had_pair || pc[i].f.hold()) { std::printf("FAIL: after the fight starts\n"); return false; } }
    if (verbose) std::printf("  n=%d seq %u %s\n", n, seq, act_name(expect));
    return true;
}

static bool flow_random(uint32_t seed, int n)
{
    std::mt19937 r(seed);
    Net net(seed * 31u + 7u);
    Pc pc[4];
    uint8_t slots[4] = { 0, 1, 2, 3 };
    std::shuffle(slots, slots + 4, r);
    if (n == 2 && seed % 2) { slots[0] = 0; slots[1] = 1; }   // the 2-player plan: host slot 0, joiner slot 1
    uint16_t members[4]; for (int i = 0; i < 4; i++) members[i] = (uint16_t)(1 + i + (r() % 3) * 4);
    const uint64_t sid = ((uint64_t)r() << 32) | r();
    jspg::Config pcf; pcf.idle_ms = 3600000; jspk::Config kcf; kcf.idle_ms = 600000;
    for (int i = 0; i < n; i++) { if (!pc[i].f.open(sid, i, n, slots)) return false; pc[i].f.set_config(pcf, kcf); }
    uint64_t t = 1000; bool had_pair = false;
    {   // FIX (pick7): the first match runs (here 30 s) while the switch handshake agrees over the same lossy link
        uint64_t t0 = 0;
        for (; t0 < 30000; t0 += 10) {
            net.deliver(t0, [&](int to, int from, const uint8_t* d, size_t len) { pc[to].f.route(from, d, len, t0); });
            if (t0 % 100 == 0) for (int i = 0; i < n; i++) pc[i].f.pump(t0, [&](const uint8_t* b, size_t len) { net.send(i, n, b, len, t0); });
        }
        for (int i = 0; i < n; i++) if (!pc[i].f.agreed()) { std::printf("FAIL: PC %d of %d not agreed after 30 s\n", i, n); return false; }
        t = t0;
    }
    // match 1 ends: Fight again without an earlier pick (the native roster stands), then random rounds, then a forced Select character and two Fight agains
    if (!flow_round(r, net, pc, n, 1, jspg::REMATCH, slots, members, &t, &had_pair, false)) return false;
    for (uint16_t s = 2; s < 5; s++) if (!flow_round(r, net, pc, n, s, jspg::NONE, slots, members, &t, &had_pair, false)) return false;
    if (!flow_round(r, net, pc, n, 5, jspg::CHARSELECT, slots, members, &t, &had_pair, false)) return false;
    PairOut before; pc[0].f.pair(&before);
    for (uint16_t s = 6; s < 8; s++) if (!flow_round(r, net, pc, n, s, jspg::REMATCH, slots, members, &t, &had_pair, false)) return false;
    PairOut after; pc[0].f.pair(&after);
    for (int i = 0; i < n; i++) if (!(before.pick[i] == after.pick[i])) { std::printf("FAIL: Fight again changed a pick\n"); return false; }
    return true;
}

static void test_flow_leave()
{
    for (int n = 2; n <= 4; n++) {
        Flow f[4]; uint8_t slots[4] = { 0, 1, 2, 3 };
        for (int i = 0; i < n; i++) CHECK(f[i].open(77, i, n, slots));
        for (int i = 0; i < n; i++) f[i].match_over(3, 0);
        CHECK(f[1].choose(jspg::LEAVE, 10));
        Act seen[4] = {};
        for (uint64_t t = 10; t < 3000; t += 10) {
            for (int i = 0; i < n; i++) {
                f[i].pump(t, [&](const uint8_t* b, size_t len) { for (int j = 0; j < n; j++) if (j != i) f[j].route(i, b, len, t); });
                const Act a = f[i].poll(t); if (a != Act::None) seen[i] = a;
            }
        }
        CHECK(seen[1] == Act::LeaveGiveUp);
        for (int i = 0; i < n; i++) if (i != 1) { CHECK(seen[i] == Act::LeaveClose); CHECK(!f[i].hold()); }
    }
    // the link goes silent during the prompt: LeaveClose (no give-up); idle: LeaveGiveUp
    Flow a, b; uint8_t slots[2] = { 0, 1 };
    jspg::Config pc; pc.idle_ms = 20000; jspk::Config kc;
    CHECK(a.open(5, 0, 2, slots) && b.open(5, 1, 2, slots)); a.set_config(pc, kc); b.set_config(pc, kc);
    a.match_over(1, 0);
    Act got = Act::None;
    for (uint64_t t = 0; t <= 9000 && got == Act::None; t += 100) { a.pump(t, [](const uint8_t*, size_t) {}); got = a.poll(t); }
    CHECK(got == Act::LeaveClose);
    a.match_over(2, 10000); b.match_over(2, 10000);
    got = Act::None;
    for (uint64_t t = 10000; t <= 31000 && got == Act::None; t += 100) {
        a.pump(t, [&](const uint8_t* d, size_t n) { b.route(0, d, n, t); }); b.pump(t, [&](const uint8_t* d, size_t n) { a.route(1, d, n, t); });
        got = a.poll(t); b.poll(t);
    }
    CHECK(got == Act::LeaveGiveUp);
    // the pick round loses a player (nothing heard for 8 s while picking): Lost
    Flow c, d;
    CHECK(c.open(6, 0, 2, slots) && d.open(6, 1, 2, slots)); c.set_config(pc, kc); d.set_config(pc, kc);
    c.match_over(1, 0); d.match_over(1, 0);
    CHECK(c.choose(jspg::CHARSELECT, 1) && d.choose(jspg::CHARSELECT, 1));
    Act lc = Act::None;
    for (uint64_t t = 0; t < 30000 && lc != Act::Lost; t += 100) {
        c.pump(t, [&](const uint8_t* x, size_t n) { d.route(0, x, n, t); });
        if (t < 2000) d.pump(t, [&](const uint8_t* x, size_t n) { c.route(1, x, n, t); });   // d dies after 2 s
        const Act a1 = c.poll(t); if (a1 != Act::None) lc = a1; d.poll(t);
    }
    CHECK(lc == Act::Lost && !c.hold());
}

static void test_routing()
{
    Flow f; uint8_t slots[2] = { 0, 1 };
    const uint8_t hs[] = { 'J', 'S', 'H', '1', 0 };
    const uint8_t pmsg[1 + jspg::kMsgLen] = { 'P' }, cmsg[1 + jspk::kMsgLen] = { 'C' };
    CHECK(!f.route(1, hs, sizeof hs, 0));   // a handshake message is not ours
    CHECK(f.route(1, pmsg, sizeof pmsg, 0) && f.route(1, cmsg, sizeof cmsg, 0));   // consumed (dropped) even with no link open
    CHECK(!f.route(1, nullptr, 0, 0));
    PairOut p; CHECK(!f.pair(&p) && !f.active() && !f.hold());
    CHECK(!f.open(1, 0, 1, slots) && !f.open(1, 2, 2, slots));
    const uint8_t dupl[2] = { 1, 1 }; CHECK(!f.open(1, 0, 2, dupl));
    // a pick message of an older link (another session tag) never changes this link's round
    Flow h, j, old;
    CHECK(h.open(1000, 0, 2, slots) && j.open(1000, 1, 2, slots) && old.open(999, 1, 2, slots));
    h.match_over(1, 0); j.match_over(1, 0); old.match_over(1, 0);
    CHECK(h.choose(jspg::CHARSELECT, 1) && j.choose(jspg::CHARSELECT, 1) && old.choose(jspg::CHARSELECT, 1));
    for (uint64_t t = 0; t < 1500; t += 100) {
        h.pump(t, [&](const uint8_t* d, size_t n) { j.route(0, d, n, t); old.route(0, d, n, t); });
        j.pump(t, [&](const uint8_t* d, size_t n) { h.route(1, d, n, t); });
        h.poll(t); j.poll(t);
    }
    jspk::Pick stale; stale.fighter = 0x77; old.poll(1500);
    for (uint64_t t = 1500; t < 3000; t += 100) {   // old locks 0x77 and keeps sending: h never takes it
        if (t == 1500) old.submit(stale, true);
        old.pump(t, [&](const uint8_t* d, size_t n) { h.route(1, d, n, t); });
        h.pump(t, [](const uint8_t*, size_t) {});
    }
    CHECK(h.round().state() == jspk::PICKING && !h.round().locked_of(1) && h.round().pick_of(1).fighter != 0x77);
    CHECK(h.routed() > 0 && h.sent() > 0);
}

static void test_members()
{
    // the record at a player's slot is a COM: the fallback gives the human records in record order to the players in slot order
    PairOut p; p.n = 2; p.slot[0] = 0; p.slot[1] = 1; p.self_slot = 1; p.pick[0].fighter = 0x21; p.pick[1].fighter = 0x22;
    uint8_t slots_room[2] = { 1, 3 }; uint16_t mem[2] = { 1, 2 };
    uint8_t img[jspk::kRoomSize]; make_room(img, 2, slots_room, mem, 3);
    jspk::HumanPick hp[4];
    CHECK(members_of(img, p, hp) == MemberMap::Order && hp[0].member == 1 && hp[1].member == 2);
    MemberMap how; CHECK(apply_pair(img, p, nullptr, &how) == jspk::Result::Ok && how == MemberMap::Order);
    CHECK(jspk::be16(img + jspk::kOffRecords + jspk::kRecSize * 3 + jspk::kRecFighter) == 0x22);
    // only one human record for two players: refused, the image is untouched
    uint8_t one_slot[1] = { 2 }; uint16_t one_mem[1] = { 1 };
    make_room(img, 1, one_slot, one_mem, 4);
    uint8_t keep[jspk::kRoomSize]; std::memcpy(keep, img, sizeof keep);
    CHECK(apply_pair(img, p, nullptr, &how) == jspk::Result::HumansMismatch && how == MemberMap::None && !std::memcmp(keep, img, sizeof keep));
    CHECK(key_on("1") && !key_on("0") && !key_on(nullptr) && !key_on("") && !key_on("10") && !key_on("yes"));
}

// ---- 5. the picker's texts ----
static void test_texts()
{
    // every name is printable ASCII (the menu font's range)
    for (int id = 0; id < 300; id++)
        for (const char* n : { jnames::fighter_name(id), jnames::support_name(id) })
            if (n) for (const char* c = n; *c; c++) CHECK(*c >= 0x20 && *c <= 0x7E);
    // fighters: Random, then the stock list's named ids in its order, once each; unnamed ids dropped
    const uint32_t stock[] = { 7, 4, 99, 7, 0, 41, 42, 0x133, 0xFFFFFFFEu };
    uint16_t out[64];
    size_t n = picker_fighters(stock, sizeof stock / sizeof stock[0], out, 64);
    CHECK(n == 5 && out[0] == jspk::kRandom && out[1] == 7 && out[2] == 4 && out[3] == 0 && out[4] == 41);
    CHECK(picker_fighters(stock, 9, out, 3) == 3);
    // no named id in the list: every named fighter alphabetically
    const uint32_t none[] = { 99, 100 };
    n = picker_fighters(none, 2, out, 64);
    CHECK(n == 43 && out[0] == jspk::kRandom && fighter_text(out[1]) == "Ace" && fighter_text(out[42]) == "Zebra");
    for (size_t i = 2; i < n; i++) CHECK(fighter_text(out[i - 1]) < fighter_text(out[i]));
    // supports: Random, then the 13 offered ones alphabetically (not the stub 208)
    n = picker_supports(out, 64);
    CHECK(n == 14 && out[0] == jspk::kRandom && support_text(out[1]) == "Allen" && support_text(out[13]) == "SKET Dance");
    for (size_t i = 1; i < n; i++) CHECK(out[i] != jnames::kSupportStub);
    CHECK(fighter_text(jspk::kRandom) == "Random" && fighter_text(7) == "Goku" && fighter_text(77) == "No. 77" && support_text(202) == "Rukia");
    // cut to a width (10 px a character here): whole when it fits, else cut with "..." and no trailing space; a 0 width keeps it whole
    auto w10 = [](const std::string& s) { return 10.0f * (float)s.size(); };
    CHECK(fit_text("Gintoki Sakata", 140, w10) == "Gintoki Sakata");
    CHECK(fit_text("Gintoki Sakata", 100, w10) == "Gintoki...");
    CHECK(fit_text("Kagura & Sadaharu", 95, w10) == "Kagura...");
    CHECK(fit_text("ab", 5, w10) == "...");
    CHECK(fit_text("caf\xC3\xA9", 1000, w10) == "caf??");
    CHECK(fit_text("Gintoki Sakata", 10, [](const std::string&) { return 0.0f; }) == "Gintoki Sakata");
    // the longest picker row and pick line at the widest plausible glyph (22 px, the cap height at scale 1) stay readable when cut
    jspk::Pick p; p.fighter = 31; p.support = 206;
    CHECK(pick_line(1, 0, p, true) == "P2: Arale & Gatchan + Kagura & Sadaharu, locked");
    CHECK(pick_line(0, 0, jspk::Pick(), false) == "P1 (you): Random + room's support, choosing");
    const std::string cut = fit_text(pick_line(1, 0, p, true), 560, [](const std::string& s) { return 22.0f * 0.7f * (float)s.size(); });
    CHECK(cut.size() >= 20 && cut.compare(cut.size() - 3, 3, "...") == 0);
}

// ---- review fixes: the host's start vs a late lock; spoofed player indexes ----
static void test_late_lock()
{
    // two PCs; the joiner locks late and the host's answer to it is held back: the host cannot start, nothing is committed early, and the
    // joiner's early "in the room" goes out only once its round is READY
    Flow h, j; uint8_t slots[2] = { 0, 1 };
    jspg::Config pc; pc.idle_ms = 600000; jspk::Config kc; kc.idle_ms = 600000;
    CHECK(h.open(42, 0, 2, slots) && j.open(42, 1, 2, slots)); h.set_config(pc, kc); j.set_config(pc, kc);
    uint64_t t = 0;
    auto both = [&](bool h_to_j, bool j_to_h) {
        h.pump(t, [&](const uint8_t* d, size_t n) { if (h_to_j) j.route(0, d, n, t); });
        j.pump(t, [&](const uint8_t* d, size_t n) { if (j_to_h) h.route(1, d, n, t); });
        h.poll(t); j.poll(t); t += 100;
    };
    h.match_over(1, 0); j.match_over(1, 0);
    CHECK(h.choose(jspg::CHARSELECT, 1) && j.choose(jspg::CHARSELECT, 1));
    for (int k = 0; k < 20; k++) both(true, true);
    CHECK(h.pick_on() && j.pick_on() && h.block_start() && !j.block_start());   // only the host's start is held
    jspk::Pick hp; hp.fighter = 7; jspk::Pick jp; jp.fighter = 4;
    CHECK(h.submit(hp, true));
    h.in_room(true); j.in_room(true);   // both in the room early
    for (int k = 0; k < 10; k++) both(false, true);   // from here the host's messages do not reach the joiner
    CHECK(!h.start_ok() && h.block_start() && !h.round().in_room_of(1) && !h.round().in_room_of(0));   // nobody READY: no flag out
    CHECK(j.submit(jp, true));
    both(false, true);   // the joiner's lock reaches the host, the host's answer does not reach the joiner
    PairOut p;
    for (int k = 0; k < 10; k++) {
        both(false, true);
        CHECK(h.round().state() != jspk::READY && !h.start_ok() && h.block_start());   // host holds all locked picks, but the joiner has not seen them
        CHECK(!h.pair(&p) && !j.pair(&p) && h.round().locked_count() == 2);       // nothing committed on either PC
    }
    for (int k = 0; k < 10 && !h.start_ok(); k++) both(true, true);
    CHECK(h.start_ok() && !h.block_start() && j.round().state() == jspk::READY && h.round().in_room_of(1));
    PairOut ph, pj; CHECK(h.pair(&ph) && j.pair(&pj));
    CHECK(ph.n == 2 && pj.n == 2 && ph.pick[0] == pj.pick[0] && ph.pick[1] == pj.pick[1] && ph.pick[0].fighter == 7 && ph.pick[1].fighter == 4);
    // a round that never became READY here (the fight started by hand anyway) leaves the previous pair
    h.fight_running(); j.fight_running();
    h.match_over(2, t); j.match_over(2, t);
    CHECK(h.choose(jspg::CHARSELECT, t) && j.choose(jspg::CHARSELECT, t));
    for (int k = 0; k < 20; k++) both(true, true);
    jspk::Pick other; other.fighter = 9; CHECK(h.submit(other, true));
    h.fight_running();
    CHECK(h.pair(&p) && p.pick[0].fighter == 7);   // still the committed pick of match 1
    CHECK(h.active());
}
static void test_spoofed()
{
    // three PCs: PC 2 sends a pick that claims to be player 1's, and a group choice that claims to be player 1's
    Flow f[3]; uint8_t slots[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; i++) CHECK(f[i].open(77, i, 3, slots));
    for (int i = 0; i < 3; i++) f[i].match_over(1, 0);
    GroupMsg g; g.session = jspk::session_tag(77); g.seq = 1; g.rev = 5; g.pick = jspg::LEAVE; g.player = 1;
    uint8_t gb[1 + kGroupMsgLen]; gb[0] = kTagPostgame; group_encode(g, gb + 1);
    CHECK(f[0].route(2, gb, sizeof gb, 10));
    CHECK(f[0].spoofed() == 1 && f[0].postgame().outcome() == jspg::PENDING);   // a forged LEAVE does not end the round
    CHECK(f[0].route(1, gb, sizeof gb, 10) && f[0].postgame().outcome() == jspg::O_LEAVE);   // the real player 1's does
    CHECK(f[0].route(0, gb, sizeof gb, 10) && f[0].spoofed() == 2);   // from this PC's own peer id
    CHECK(f[0].route(5, gb, sizeof gb, 10) && f[0].spoofed() == 3);   // from no player of the link
    // picks: CharSelect on all three, then a forged lock for player 1 from peer 2
    for (int i = 0; i < 3; i++) { f[i].match_over(2, 100); CHECK(f[i].choose(jspg::CHARSELECT, 100)); }
    for (uint64_t t = 100; t < 2000; t += 100)
        for (int i = 0; i < 3; i++) { f[i].pump(t, [&](const uint8_t* d, size_t n) { for (int k = 0; k < 3; k++) if (k != i) f[k].route(i, d, n, t); }); f[i].poll(t); }
    CHECK(f[0].round().state() == jspk::PICKING);
    jspk::Msg m; m.session = jspk::session_tag(77); m.seq = 2; m.rev = 500; m.pick.fighter = 0x13; m.locked = true; m.player = 1; m.saw = 7;
    uint8_t cb[1 + jspk::kMsgLen]; cb[0] = kTagPick; jspk::encode(m, cb + 1);
    const uint64_t before = f[0].spoofed();
    CHECK(f[0].route(2, cb, sizeof cb, 2000));
    CHECK(f[0].spoofed() == before + 1 && !f[0].round().locked_of(1) && f[0].round().pick_of(1).fighter != 0x13);
    CHECK(f[0].route(1, cb, sizeof cb, 2000) && f[0].round().locked_of(1) && f[0].round().pick_of(1).fighter == 0x13);   // from its real sender
    // 2 players: the 'P' message has no player field; only the one other peer may send it
    Flow a; uint8_t s2[2] = { 0, 1 }; CHECK(a.open(5, 0, 2, s2)); a.match_over(1, 0);
    jspg::Msg pm; pm.seq = 1; pm.rev = 3; pm.pick = jspg::LEAVE; uint8_t pb[1 + jspg::kMsgLen]; pb[0] = kTagPostgame; jspg::encode(pm, pb + 1);
    CHECK(a.route(0, pb, sizeof pb, 1) && a.spoofed() == 1 && a.postgame().outcome() == jspg::PENDING);
    CHECK(a.route(1, pb, sizeof pb, 1) && a.postgame().outcome() == jspg::O_LEAVE);
}
// the roster write ignores the record team bytes (a PC whose versus_apply swapped them writes the same roster) and leaves R+0x08 / +0x0C
static void test_apply_teams()
{
    uint8_t slots[2] = { 0, 1 }; uint16_t mem[2] = { 1, 2 };
    uint8_t a[jspk::kRoomSize], b2[jspk::kRoomSize]; make_room(a, 2, slots, mem, 11); std::memcpy(b2, a, sizeof a);
    // b2: as if this PC's versus_apply swapped the second human with a COM of the other team
    std::swap(b2[jspk::kOffRecords + jspk::kRecSize * 1 + jspk::kRecTeam], b2[jspk::kOffRecords + jspk::kRecSize * 2 + jspk::kRecTeam]);
    b2[jspk::kOffSelfFighter + 3] ^= 0x5A;   // and its own pick block differs (it is per PC)
    const uint8_t self_a = a[jspk::kOffSelfFighter + 3], self_b = b2[jspk::kOffSelfFighter + 3];
    for (int mode = 0; mode < 2; mode++) {
        uint8_t x[jspk::kRoomSize], y[jspk::kRoomSize]; std::memcpy(x, a, sizeof a); std::memcpy(y, b2, sizeof b2);
        PairOut p; p.n = 2; p.slot[0] = 0; p.slot[1] = 1; p.pick[0].fighter = 3; p.pick[1].fighter = 5; p.pick[0].mode = mode ? jspk::TOGETHER : jspk::VERSUS;
        p.self_slot = 0; CHECK(apply_pair(x, p) == jspk::Result::Ok);
        p.self_slot = 1; CHECK(apply_pair(y, p) == jspk::Result::Ok);
        for (uint32_t i = 0; i < jspk::kRoomSize; i++) if (i != jspk::kOffSelfFighter + 3) CHECK(x[i] == y[i]);
        CHECK(x[jspk::kOffSelfFighter + 3] == self_a && y[jspk::kOffSelfFighter + 3] == self_b);   // not written
        const uint8_t t0 = x[jspk::kOffRecords + jspk::kRecTeam], t1 = x[jspk::kOffRecords + jspk::kRecSize + jspk::kRecTeam];
        CHECK(t0 == 0 && (mode ? t1 == 0 : t1 == 1));
    }
}

// ---- FIX (pick7): the switch handshake (JSP_ROLLBACK_PICK on one PC only must not leave it in a prompt) ----
static void test_hello_wire()
{
    Hello h; h.session = 0x12345678u; h.heard = 0x5; h.player = 2; h.declined = 0x1234;
    uint8_t b[kHelloLen]; hello_encode(h, b);
    Hello o; CHECK(hello_decode(b, kHelloLen, &o) && o.session == h.session && o.heard == 5 && o.player == 2 && o.declined == 0x1234);
    for (size_t i = 0; i < kHelloLen; i++) for (int bit = 0; bit < 8; bit++) { uint8_t c[kHelloLen]; std::memcpy(c, b, sizeof c); c[i] ^= (uint8_t)(1u << bit); CHECK(!hello_decode(c, kHelloLen, &o)); }
    CHECK(!hello_decode(b, kHelloLen - 1, &o) && !hello_decode(nullptr, kHelloLen, &o));
    Hello z; z.session = 0; uint8_t zb[kHelloLen]; hello_encode(z, zb); CHECK(!hello_decode(zb, kHelloLen, &o));   // session tag 0 never
    // a hello is never a choice message, and the choice messages are never hellos (length and magic)
    jspg::Msg pm; CHECK(!jspg::decode(b, kHelloLen, &pm));
    GroupMsg gm; CHECK(!group_decode(b, kHelloLen, &gm));
    uint8_t p8[jspg::kMsgLen]; pm.seq = 1; pm.rev = 2; jspg::encode(pm, p8); CHECK(!hello_decode(p8, jspg::kMsgLen, &o));
    uint8_t g14[kGroupMsgLen]; gm.session = 7; group_encode(gm, g14); CHECK(!hello_decode(g14, kGroupMsgLen, &o));
}
// a and b are players 0 and 1 of one link; a_to_b / b_to_a: whether this tick's messages arrive
static void pump2(Flow& a, Flow& b, uint64_t t, bool a_to_b = true, bool b_to_a = true)
{
    a.pump(t, [&](const uint8_t* d, size_t n) { if (a_to_b) b.route(0, d, n, t); });
    b.pump(t, [&](const uint8_t* d, size_t n) { if (b_to_a) a.route(1, d, n, t); });
}
static void test_switch()
{
    uint8_t slots[2] = { 0, 1 };
    jspg::Config pc; jspk::Config kc;   // the shipped timeouts (prompt idle 180 s)
    {   // only A has JSP_ROLLBACK_PICK=1: B's Flow stays closed (jsp_net_game.cpp opens it only with the switch on)
        Flow a, b; CHECK(a.open(900, 0, 2, slots)); a.set_config(pc, kc);
        for (uint64_t t = 0; t < 30000; t += 100) pump2(a, b, t);
        CHECK(!a.agreed() && !b.is_open() && b.off_hellos() >= 25);   // B saw A's hellos (logged once there) and sent nothing
        CHECK(!a.match_end(1, 30000) && a.decline_why() == 1);           // no prompt on A: both behave as off
        CHECK(!a.hold() && !a.postgame().on() && a.poll(30000) == Act::None && !a.active());
        for (uint64_t t = 30000; t < 300000; t += 100) { pump2(a, b, t); CHECK(a.poll(t) == Act::None); }
        CHECK(!a.hold());
        // before pick7 the same match end opened the prompt on A and held it for the idle timeout (the stall): the link is alive (B's other
        // traffic), so only idle_ms ends it
        Flow old; CHECK(old.open(900, 0, 2, slots)); old.set_config(pc, kc);
        old.match_over(1, 0);
        const uint8_t hs[] = { 'J', 'S', 'H', '1', 0 };
        uint64_t left_at = 0;
        for (uint64_t t = 0; t < 300000 && !left_at; t += 100) { old.route(1, hs, sizeof hs, t); old.pump(t, [](const uint8_t*, size_t) {}); if (old.poll(t) != Act::None) left_at = t; }
        CHECK(left_at >= 179000);
    }
    {   // both on, a clean link: agreed within 2 s, the prompt opens on both
        Flow a, b; CHECK(a.open(901, 0, 2, slots) && b.open(901, 1, 2, slots)); a.set_config(pc, kc); b.set_config(pc, kc);
        uint64_t t = 0;
        for (; t < 10000 && !(a.agreed() && b.agreed()); t += 100) pump2(a, b, t);
        CHECK(a.agreed() && b.agreed() && t <= 2000);
        CHECK(a.match_end(1, t) && b.match_end(1, t) && a.hold() && b.hold());
        CHECK(a.choose(jspg::REMATCH, t) && b.choose(jspg::REMATCH, t));
        Act aa = Act::None, ab = Act::None;
        for (uint64_t e = t + 2000; t < e; t += 100) { pump2(a, b, t); const Act x = a.poll(t), y = b.poll(t); if (x != Act::None && aa == Act::None) aa = x; if (y != Act::None && ab == Act::None) ab = y; }
        CHECK(aa == Act::Rematch && ab == Act::Rematch);
    }
    {   // both on, but B never got A's hello that says A heard B (lost at the end of the match): A agreed, B did not
        Flow a, b; CHECK(a.open(902, 0, 2, slots) && b.open(902, 1, 2, slots)); a.set_config(pc, kc); b.set_config(pc, kc);
        pump2(a, b, 0, true, false);   // A's first hello (heard nothing) reaches B; B's first reaches nobody
        for (uint64_t t = 100; t < 5000; t += 100) pump2(a, b, t, false, true);   // from here only B -> A
        CHECK(a.agreed() && !b.agreed());
        CHECK(a.match_end(1, 5000) && a.hold());                                  // A opens its prompt ...
        CHECK(!b.match_end(1, 5000) && b.decline_why() == 1 && !b.hold());        // ... B declines it (its next hello says so at once)
        uint64_t off_at = 0;
        for (uint64_t t = 5000; t < 8000 && !off_at; t += 100) { pump2(a, b, t); if (a.poll(t) == Act::PeerOff) off_at = t; b.poll(t); }
        CHECK(off_at && off_at <= 5200 && !a.hold() && !a.postgame().on());       // A's prompt closed within one hello
        for (uint64_t t = 8000; t < 9000; t += 100) { pump2(a, b, t); CHECK(a.poll(t) == Act::None); }   // once
        CHECK(a.agreed() && b.agreed());                                          // the next match has the menu on both
        CHECK(a.match_end(2, 9000) && b.match_end(2, 9000) && a.hold() && b.hold());
    }
    {   // B's decline reaches A before A's own match end: A does not open its prompt at all
        Flow a, b; CHECK(a.open(903, 0, 2, slots) && b.open(903, 1, 2, slots)); a.set_config(pc, kc); b.set_config(pc, kc);
        pump2(a, b, 0, true, false);
        for (uint64_t t = 100; t < 5000; t += 100) pump2(a, b, t, false, true);
        CHECK(!b.match_end(4, 5000));
        for (uint64_t t = 5000; t < 5300; t += 100) pump2(a, b, t, false, true);
        CHECK(!a.match_end(4, 5300) && a.decline_why() == 2 && !a.hold() && a.poll(5300) == Act::None);
    }
    {   // an older link's hello and a forged sender change nothing
        Flow a, b, old; CHECK(a.open(904, 0, 2, slots) && b.open(904, 1, 2, slots) && old.open(903, 1, 2, slots));
        for (uint64_t t = 0; t < 5000; t += 100) old.pump(t, [&](const uint8_t* d, size_t n) { a.route(1, d, n, t); });
        CHECK(!a.agreed());
        Hello h; h.session = jspk::session_tag(904); h.player = 0; h.heard = 3; uint8_t hb[1 + kHelloLen]; hb[0] = kTagPostgame; hello_encode(h, hb + 1);
        const uint64_t sp = a.spoofed();
        CHECK(a.route(1, hb, sizeof hb, 10) && a.spoofed() == sp + 1 && !a.agreed());   // claims player 0 from peer 1
    }
}

int main()
{
    test_hello_wire();
    test_switch();
    test_group_wire();
    test_group_states();
    int bad = 0;
    for (int n = 2; n <= 4; n++) for (uint32_t s = 1; s <= 300; s++) if (!group_random(s * 104729u + (uint32_t)n, n)) { if (bad++ < 3) std::printf("FAIL group random n=%d seed %u\n", n, s); fails++; }
    test_postgame_two();
    test_routing();
    test_members();
    test_texts();
    test_late_lock();
    test_spoofed();
    test_apply_teams();
    test_flow_leave();
    int fbad = 0, runs = 0;
    for (int n = 2; n <= 4; n++) for (uint32_t s = 1; s <= 60; s++, runs++) if (!flow_random(s * 7919u + (uint32_t)n * 13u, n)) { if (fbad++ < 3) std::printf("FAIL flow run n=%d seed %u\n", n, s); fails++; }
    if (fails) { std::printf("net pick flow: %d failure(s)\n", fails); return 1; }
    std::printf("net pick flow: switch handshake (one PC on, a lost last hello, an early decline, old link, forged sender), group choice (900 random runs), 2-player path, routing, spoofed senders, late lock vs the host's start, member map, team bytes, picker texts, leave / silence / idle / lost, %d flow runs of 8 post-games (n = 2..4) ok\n", runs);
    return 0;
}
