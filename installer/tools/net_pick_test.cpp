// Host test of the character pick after "Select character" (installer/port/jsp/jsp_net_pick.h): wire format, the pick agreement for
// 2..4 players (timeouts, stale messages, random loss / reordering / duplication), Random, and the roster write in both team modes,
// including that every player's PC computes the same bytes. No game, no GPU.
#include "../port/jsp/jsp_net_pick.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <random>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace jspk;

static Pick pk(uint16_t f, uint16_t s, TeamMode m = VERSUS) { Pick p; p.fighter = f; p.support = s; p.mode = m; return p; }

// ---- wire format ----
static void test_wire()
{
    for (int fl = 0; fl < 8; fl++)
        for (int pl = 0; pl < 4; pl++) {
            Msg m; m.session = 0xA1B2C3D4u; m.seq = 0x1234; m.rev = 0xFEDC; m.pick = pk(0x0102, 0x00D3, (fl & 4) ? TOGETHER : VERSUS);
            m.locked = fl & 1; m.in_room = (fl & 2) != 0; m.player = (uint8_t)pl; m.saw = (uint8_t)(0xF >> pl);
            uint8_t b[kMsgLen]; encode(m, b);
            Msg d; CHECK(decode(b, kMsgLen, &d));
            CHECK(d.session == m.session && d.seq == m.seq && d.rev == m.rev && d.pick == m.pick && d.locked == m.locked &&
                  d.in_room == m.in_room && d.player == m.player && d.saw == m.saw);
            for (size_t n = 0; n < kMsgLen; n++) CHECK(!decode(b, n, &d));   // truncated
            uint8_t big[kMsgLen + 1]; std::memcpy(big, b, kMsgLen); big[kMsgLen] = 0; CHECK(!decode(big, kMsgLen + 1, &d));
            for (size_t i = 0; i < kMsgLen; i++)   // any single changed byte is refused (check byte, magic, reserved bits)
                for (int bit = 0; bit < 8; bit++) { uint8_t c[kMsgLen]; std::memcpy(c, b, kMsgLen); c[i] ^= (uint8_t)(1 << bit); CHECK(!decode(c, kMsgLen, &d)); }
        }
    Msg d;
    CHECK(!decode(nullptr, kMsgLen, &d));
    Msg z; z.session = 0; uint8_t b[kMsgLen]; encode(z, b); CHECK(!decode(b, kMsgLen, &d));   // session tag 0
    // reserved bits with a valid check byte
    Msg m; m.session = 7; encode(m, b);
    for (int at : { 14, 15, 16 }) {
        uint8_t c[kMsgLen]; std::memcpy(c, b, kMsgLen); c[at] |= 0x80; c[17] ^= 0x80; CHECK(!decode(c, kMsgLen, &d));
    }
    // garbage: random bytes are refused (only a valid check byte with the right magic/version could pass)
    std::mt19937 r(5); int passed = 0;
    for (int i = 0; i < 20000; i++) { uint8_t g[kMsgLen]; for (auto& x : g) x = (uint8_t)r(); if (decode(g, kMsgLen, &d)) passed++; }
    CHECK(passed == 0);
    CHECK(session_tag(0) == 1 && session_tag(0x0000000100000001ull) == 1 && session_tag(0x12345678ull) == 0x12345678u);
    CHECK(idle_ms_from(nullptr) == kIdleMs && idle_ms_from("") == kIdleMs && idle_ms_from("29") == kIdleMs && idle_ms_from("30") == 30000 &&
          idle_ms_from("600") == 600000 && idle_ms_from("601") == kIdleMs && idle_ms_from("4x") == kIdleMs && idle_ms_from("99999999999") == kIdleMs);
}

// ---- the state machine (2 players unless said) ----
static Msg out_of(PickAgreement& a, uint64_t now) { Msg m; CHECK(a.take(now, &m)); return m; }

static void test_states()
{
    const uint32_t S = 0x55AA;
    PickAgreement h, j;
    CHECK(h.state() == IDLE);
    Msg dummy; CHECK(!h.take(0, &dummy));
    CHECK(!h.start(S, 3, 0, 1, Pick(), 0) && !h.start(S, 3, 2, 2, Pick(), 0) && !h.start(S, 3, 0, 5, Pick(), 0));
    CHECK(h.start(S, 3, 0, 2, Pick(), 0) && j.start(S, 3, 1, 2, pk(7, 0xC9), 0));
    CHECK(h.state() == PICKING && h.seconds_left(0) == 120);
    CHECK(h.set_pick(pk(4, 0xC8, TOGETHER)) && h.lock());
    CHECK(h.state() == LOCKED && !h.set_pick(pk(5, 5)) && !h.lock());
    CHECK(j.receive(out_of(h, 10), 10) && j.locked_of(0) && j.pick_of(0) == pk(4, 0xC8, TOGETHER));
    CHECK(std::strcmp(j.status(), "Opponent is ready") == 0);
    CHECK(j.lock() && j.state() == LOCKED);               // j holds both, but h has not said it holds j's yet
    CHECK(h.receive(out_of(j, 20), 20) && h.state() == READY);   // j's message: locked + saw both
    CHECK(j.receive(out_of(h, 30), 30) && j.state() == READY);
    Pick ph[kMaxPlayers], pj[kMaxPlayers];
    CHECK(h.picks(ph) && j.picks(pj));
    for (int i = 0; i < 2; i++) CHECK(ph[i] == pj[i]);
    CHECK(ph[1] == pk(7, 0xC9) && agreed_mode(ph) == TOGETHER);
    // resend timing: nothing new within resend_ms, then the same state again; stop() ends sending
    Msg m; CHECK(h.take(300, &m)); CHECK(!h.take(400, &m)); CHECK(h.take(560, &m));
    h.stop(); CHECK(!h.take(10000, &m) && h.state() == IDLE);

    // idle auto-lock: the start pick (the previous one) is locked as it is
    PickAgreement a; Config c; c.idle_ms = 30000;
    a.start(S, 1, 1, 2, pk(9, 0xCA), 0, c);
    for (uint64_t t = 0; t < 30000; t += 1000) { Msg p; p.session = S; p.seq = 1; p.rev = (uint16_t)(1 + t / 1000); p.player = 0; a.receive(p, t); a.tick(t); }
    CHECK(a.state() == PICKING && a.seconds_left(29500) == 1);
    a.tick(30000);
    CHECK(a.state() == LOCKED && a.auto_locked() && a.mine() == pk(9, 0xCA));
    // locked, the peer alive but never locking: LOST after idle + wait
    for (uint64_t t = 30000; t < 90000; t += 1000) { Msg p; p.session = S; p.seq = 1; p.rev = (uint16_t)(100 + t / 1000); p.player = 0; a.receive(p, t); a.tick(t); }
    CHECK(a.state() == LOCKED);
    a.tick(90000);
    CHECK(a.state() == LOST && a.reason() == R_PEER_TIMEOUT);
    CHECK(a.take(90000, &m) && m.locked); CHECK(!a.take(92000, &m));   // linger, then silent

    // silence: nothing heard for 8 s
    PickAgreement b; b.start(S, 1, 0, 2, Pick(), 0);
    b.tick(7999); CHECK(b.state() == PICKING);
    b.tick(8000); CHECK(b.state() == LOST && b.reason() == R_LINK_LOST);
    // a peer that is alive keeps it going
    PickAgreement e; e.start(S, 1, 0, 2, Pick(), 0);
    for (uint64_t t = 0; t <= 20000; t += 2000) { Msg p; p.session = S; p.seq = 1; p.rev = (uint16_t)(1 + t); p.player = 1; e.receive(p, t); e.tick(t); }
    CHECK(e.state() == PICKING);
    // N = 3: silence of one player loses the round
    PickAgreement f; f.start(S, 1, 0, 3, Pick(), 0);
    for (uint64_t t = 0; t <= 8000; t += 1000) { Msg p; p.session = S; p.seq = 1; p.rev = (uint16_t)(1 + t); p.player = 1; f.receive(p, t); f.tick(t); }
    CHECK(f.state() == LOST);
}

static void test_stale()
{
    const uint32_t S = 0x1111, T = 0x2222;
    PickAgreement j; j.start(S, 4, 1, 2, Pick(), 0);
    Msg m; m.session = T; m.seq = 4; m.rev = 9; m.locked = true; m.player = 0; m.pick = pk(1, 1); m.saw = 3;
    CHECK(!j.receive(m, 1) && !j.locked_of(0));            // another link
    m.session = S; m.seq = 3; CHECK(!j.receive(m, 1) && !j.locked_of(0));   // an earlier round
    m.seq = 4; m.player = 1; CHECK(!j.receive(m, 1));      // an echo of this side
    m.player = 3; CHECK(!j.receive(m, 1));                 // not a player of a 2-player round
    m.player = 0; CHECK(j.receive(m, 1) && j.locked_of(0));
    Msg older = m; older.rev = 5; older.pick = pk(2, 2); CHECK(j.receive(older, 2) && j.pick_of(0) == pk(1, 1));
    Msg changed = m; changed.rev = 10; changed.pick = pk(3, 3); CHECK(j.receive(changed, 3) && j.pick_of(0) == pk(1, 1) && j.conflicts() == 1);
    // a later round's message arriving early is kept aside and taken at start()
    Msg early = m; early.seq = 5; early.rev = 2; early.pick = pk(6, 6);
    CHECK(!j.receive(early, 4));
    j.start(S, 5, 1, 2, Pick(), 5);
    CHECK(j.locked_of(0) && j.pick_of(0) == pk(6, 6));
    // bytes path
    uint8_t b[kMsgLen]; Msg nx = early; nx.rev = 3; encode(nx, b);
    CHECK(j.receive_bytes(b, kMsgLen, 6)); CHECK(!j.receive_bytes(b, kMsgLen - 1, 6));
}

// ---- random traffic for N players: loss, reordering, duplicates, picks and locks at random times ----
static bool random_run(uint32_t seed, int n)
{
    std::mt19937 r(seed);
    const uint32_t S = 0xBEEF;
    PickAgreement p[kMaxPlayers];
    struct Fl { uint64_t at; Msg m; int to; };
    std::vector<Fl> fl;
    uint64_t lock_at[kMaxPlayers];
    for (int i = 0; i < n; i++) { p[i].start(S, 2, i, n, pk((uint16_t)(10 + i), (uint16_t)(0xC8 + i)), (uint64_t)(r() % 500)); lock_at[i] = 500 + r() % 5000; }
    for (uint64_t t = 500; t < 30000; t += 10) {
        for (int i = 0; i < n; i++) {
            if (t == (lock_at[i] / 10) * 10 - 200) p[i].set_pick(pk((uint16_t)(20 + r() % 30), (uint16_t)(0xC8 + r() % 10), (r() & 1) ? TOGETHER : VERSUS));
            if (t >= lock_at[i] && p[i].state() == PICKING) p[i].lock();
            Msg m;
            if (p[i].take(t, &m))
                for (int k = 0; k < n; k++) {
                    if (k == i || r() % 100 < 30) continue;                     // 30 % loss
                    const int copies = r() % 100 < 10 ? 2 : 1;                  // 10 % duplicates
                    for (int c = 0; c < copies; c++) fl.push_back({ t + 20 + r() % 300, m, k });   // delay + reordering
                }
        }
        for (size_t f = 0; f < fl.size();) {
            if (fl[f].at <= t) { p[fl[f].to].receive(fl[f].m, t); fl[f] = fl.back(); fl.pop_back(); } else f++;
        }
        bool all = true;
        for (int i = 0; i < n; i++) { p[i].tick(t); if (p[i].state() != READY) all = false; }
        if (all) break;
    }
    Pick ref[kMaxPlayers];
    if (!p[0].picks(ref)) return false;
    for (int i = 1; i < n; i++) {
        Pick o[kMaxPlayers];
        if (!p[i].picks(o)) return false;
        for (int k = 0; k < n; k++) if (!(o[k] == ref[k])) return false;
    }
    for (int i = 0; i < n; i++) if (p[i].conflicts()) return false;
    return true;
}

// ---- the roster ----
// A room image: records with member ids (0 = COM), teams, fighters, supports; the per-PC block filled with junk of this PC.
struct Rec { uint16_t member; uint8_t team; uint16_t fighter; uint16_t support; };
static void make_room(uint8_t img[kRoomSize], const Rec (&rec)[4], uint32_t ts0, uint32_t ts1, uint32_t pc_seed)
{
    std::mt19937 r(pc_seed);
    for (size_t i = 0; i < kRoomSize; i++) img[i] = (uint8_t)(0x40 + i);   // shared bytes
    for (size_t i = 0; i < 0x34; i++) img[i] = (uint8_t)r();               // this PC's own pick block and local fields differ per PC
    put_be32(img + kOffTeamSupport0, ts0); put_be32(img + kOffTeamSupport1, ts1);
    for (int i = 0; i < 4; i++) {
        uint8_t* p = img + kOffRecords + kRecSize * i;
        put_be16(p + kRecMember, rec[i].member); p[kRecTeam] = rec[i].team; put_be16(p + kRecFighter, rec[i].fighter);
        put_be16(p + kRecSupport, rec[i].support); p[kRecCom] = rec[i].member ? 0 : 1;
        img[kOffSlotTeam + i] = rec[i].team;
    }
}
static uint8_t team_of(const uint8_t* img, int i) { return img[kOffRecords + kRecSize * i + kRecTeam]; }
static uint16_t fighter_of(const uint8_t* img, int i) { return be16(img + kOffRecords + kRecSize * i + kRecFighter); }
static uint16_t support_of(const uint8_t* img, int i) { return be16(img + kOffRecords + kRecSize * i + kRecSupport); }
static uint32_t team_support(const uint8_t* img, int t) { return be32(img + (t ? kOffTeamSupport1 : kOffTeamSupport0)); }
static bool balanced(const uint8_t* img) { int c[2] = { 0, 0 }; for (int i = 0; i < 4; i++) { if (team_of(img, i) > 1) return false; c[team_of(img, i)]++; } return c[0] == 2 && c[1] == 2; }
static bool supports_follow_team(const uint8_t* img) { for (int i = 0; i < 4; i++) if (support_of(img, i) != (uint16_t)team_support(img, team_of(img, i))) return false; return true; }

// Every PC (own junk in the per-PC block, own self member) gets the same bytes outside its own pick block; the block holds its own pick.
static void check_symmetry(const Rec (&rec)[4], const HumanPick* hp, int n, TeamMode mode, int line)
{
    uint8_t ref[kRoomSize]; bool have = false;
    for (int pc = 0; pc < n; pc++) {
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 100 + pc);
        uint8_t before[kRoomSize]; std::memcpy(before, img, kRoomSize);
        // the picks in another order on each PC: the order must not matter
        HumanPick order[kMaxPlayers]; for (int i = 0; i < n; i++) order[i] = hp[(i + pc) % n];
        if (roster_apply_image(img, order, n, mode, hp[pc].member) != Result::Ok) { std::printf("FAIL line %d: refused on PC %d\n", line, pc); fails++; return; }
        for (size_t i = 0x34; i < kRoomSize; i++) if (!have) ref[i] = img[i]; else if (ref[i] != img[i]) { std::printf("FAIL line %d: PC %d byte %zX differs\n", line, pc, i); fails++; return; }
        have = true;
        for (size_t i = 0; i < 0x34; i++)   // only the own fighter / support words of the per-PC block change
            if ((i < kOffSelfFighter || i >= kOffSelfSupport + 4) && img[i] != before[i]) { std::printf("FAIL line %d: PC %d per-PC byte %zX changed\n", line, pc, i); fails++; return; }
        int myrec = -1; for (int r = 0; r < 4; r++) if (rec[r].member == hp[pc].member) myrec = r;
        if (be32(img + kOffSelfFighter) != fighter_of(img, myrec) || be32(img + kOffSelfSupport) != team_support(img, team_of(img, myrec))) {
            std::printf("FAIL line %d: PC %d self pick block\n", line, pc); fails++;
        }
        // idempotent: a second write gives the same bytes
        uint8_t again[kRoomSize]; std::memcpy(again, img, kRoomSize);
        roster_apply_image(again, order, n, mode, hp[pc].member);
        if (std::memcmp(again, img, kRoomSize)) { std::printf("FAIL line %d: not idempotent on PC %d\n", line, pc); fails++; }
    }
}

static void test_roster_two()
{
    const HumanPick hp[2] = { { 1, pk(4, 0xC8) }, { 2, pk(9, 0xD0) } };
    // versus, both humans on team 0: the joiner goes to team 1, a COM comes over; each human's team gets that human's support
    {
        const Rec rec[4] = { { 1, 0, 0x133, 0 }, { 2, 0, 0x133, 0 }, { 0, 1, 30, 0 }, { 0, 1, 31, 0 } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        uint8_t before[kRoomSize]; std::memcpy(before, img, kRoomSize);
        ApplyInfo ai;
        CHECK(roster_apply_image(img, hp, 2, VERSUS, 0, &ai) == Result::Ok);
        CHECK(team_of(img, 0) == 0 && team_of(img, 1) == 1 && balanced(img) && ai.moved == 2);
        CHECK(team_of(img, 2) == 1 && team_of(img, 3) == 0);   // the first COM keeps its team while there is room
        CHECK(img[kOffSlotTeam + 1] == 1 && img[kOffSlotTeam + 3] == 0 && img[kOffSlotTeam + 0] == 0 && img[kOffSlotTeam + 2] == 1);
        CHECK(fighter_of(img, 0) == 4 && fighter_of(img, 1) == 9 && fighter_of(img, 2) == 30 && fighter_of(img, 3) == 31);
        CHECK(team_support(img, 0) == 0xC8 && team_support(img, 1) == 0xD0 && supports_follow_team(img));
        CHECK(std::memcmp(img, before, kOffSelfFighter) == 0);   // self_member 0: the per-PC block untouched
        for (int i = 0; i < 4; i++)   // the COM records: only team and support (the team's) may differ
            for (uint32_t b = 0; b < kRecSize; b++)
                if (i >= 2 && b != kRecTeam && b != kRecSupport && b != kRecSupport + 1) CHECK(img[kOffRecords + kRecSize * i + b] == before[kOffRecords + kRecSize * i + b]);
        check_symmetry(rec, hp, 2, VERSUS, __LINE__);
    }
    // versus, already opposite (host on team 1): no team byte changes
    {
        const Rec rec[4] = { { 0, 0, 30, 0 }, { 1, 1, 5, 0 }, { 2, 0, 6, 0 }, { 0, 1, 31, 0 } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        ApplyInfo ai;
        CHECK(roster_apply_image(img, hp, 2, VERSUS, 0, &ai) == Result::Ok && ai.moved == 0);
        for (int i = 0; i < 4; i++) CHECK(team_of(img, i) == rec[i].team && img[kOffSlotTeam + i] == rec[i].team);
        CHECK(team_support(img, 1) == 0xC8 && team_support(img, 0) == 0xD0 && supports_follow_team(img));
        check_symmetry(rec, hp, 2, VERSUS, __LINE__);
    }
    // Random: the fighter and support the host's broadcast put in the record stay
    {
        const Rec rec[4] = { { 1, 0, 17, 0xC3 }, { 2, 1, 23, 0xC4 }, { 0, 0, 30, 0 }, { 0, 1, 31, 0 } };
        const HumanPick rp[2] = { { 1, pk(kRandom, kRandom) }, { 2, pk(8, kRandom) } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        CHECK(roster_apply_image(img, rp, 2, VERSUS, 0) == Result::Ok);
        CHECK(fighter_of(img, 0) == 17 && fighter_of(img, 1) == 8 && team_support(img, 0) == 0xC3 && team_support(img, 1) == 0xC4);
        check_symmetry(rec, rp, 2, VERSUS, __LINE__);
    }
    // together, humans opposite: the joiner comes to the host's team, both COMs on the other; the shared team uses the host's support
    {
        const Rec rec[4] = { { 1, 1, 0, 0 }, { 0, 1, 30, 0 }, { 2, 0, 0, 0 }, { 0, 0, 31, 0 } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x77, 0x66, 1);
        ApplyInfo ai;
        CHECK(roster_apply_image(img, hp, 2, TOGETHER, 0, &ai) == Result::Ok);
        CHECK(team_of(img, 0) == 1 && team_of(img, 2) == 1 && team_of(img, 1) == 0 && team_of(img, 3) == 0 && balanced(img) && ai.moved == 2);
        CHECK(team_support(img, 1) == 0xC8 && team_support(img, 0) == 0x77 && supports_follow_team(img));   // COM team keeps its support
        CHECK(fighter_of(img, 0) == 4 && fighter_of(img, 2) == 9);
        check_symmetry(rec, hp, 2, TOGETHER, __LINE__);
    }
    // together, already on one team: no team byte changes
    {
        const Rec rec[4] = { { 0, 0, 30, 0 }, { 0, 0, 31, 0 }, { 2, 1, 0, 0 }, { 1, 1, 0, 0 } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        ApplyInfo ai;
        CHECK(roster_apply_image(img, hp, 2, TOGETHER, 0, &ai) == Result::Ok && ai.moved == 0);
        for (int i = 0; i < 4; i++) CHECK(team_of(img, i) == rec[i].team);
        check_symmetry(rec, hp, 2, TOGETHER, __LINE__);
    }
    // unset teams (2): rebuilt from team 0, slot bytes that say "not set" stay 2
    {
        const Rec rec[4] = { { 1, 2, 0, 0 }, { 2, 2, 0, 0 }, { 0, 2, 30, 0 }, { 0, 2, 31, 0 } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        CHECK(roster_apply_image(img, hp, 2, VERSUS, 0) == Result::Ok && balanced(img));
        CHECK(team_of(img, 0) == 0 && team_of(img, 1) == 1 && team_of(img, 0) != team_of(img, 1));
        for (int i = 0; i < 4; i++) CHECK(img[kOffSlotTeam + i] == 2);
    }
    // refusals: the native roster stands (image untouched)
    {
        const Rec one[4] = { { 1, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 1, 0, 0 } };
        const Rec ids[4] = { { 1, 0, 0, 0 }, { 5, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 1, 0, 0 } };
        const Rec three[4] = { { 1, 0, 0, 0 }, { 2, 0, 0, 0 }, { 3, 1, 0, 0 }, { 0, 1, 0, 0 } };
        for (const auto* rec : { &one, &ids, &three }) {
            uint8_t img[kRoomSize]; make_room(img, *rec, 0x133, 0x133, 1);
            uint8_t before[kRoomSize]; std::memcpy(before, img, kRoomSize);
            CHECK(roster_apply_image(img, hp, 2, VERSUS, 1) == Result::HumansMismatch);
            CHECK(std::memcmp(img, before, kRoomSize) == 0);
        }
        uint8_t img[kRoomSize]; make_room(img, one, 0x133, 0x133, 1);
        const HumanPick dup[2] = { { 1, Pick() }, { 1, Pick() } };
        const HumanPick zero[2] = { { 0, Pick() }, { 1, Pick() } };
        CHECK(roster_apply_image(img, dup, 2, VERSUS, 0) == Result::BadPicks);
        CHECK(roster_apply_image(img, zero, 2, VERSUS, 0) == Result::BadPicks);
        CHECK(roster_apply_image(img, hp, 1, VERSUS, 0) == Result::BadCount);
        CHECK(roster_apply_image(img, hp, 5, VERSUS, 0) == Result::BadCount);
        CHECK(std::strcmp(result_text(Result::HumansMismatch), "?") != 0);
    }
}

static void test_roster_more()
{
    // N = 3: versus 2 + 1, together 2 + 1 (the two lowest member ids together), one COM on the single human's team
    const HumanPick h3[3] = { { 1, pk(4, 0xC8) }, { 2, pk(9, 0xD0) }, { 3, pk(12, 0xD1) } };
    {
        const Rec rec[4] = { { 1, 0, 0, 0 }, { 2, 0, 0, 0 }, { 3, 1, 0, 0 }, { 0, 1, 30, 0 } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        ApplyInfo ai;
        CHECK(roster_apply_image(img, h3, 3, VERSUS, 0, &ai) == Result::Ok && ai.moved == 0);   // already 2 + 1
        CHECK(team_support(img, 0) == 0xC8 && team_support(img, 1) == 0xD1 && supports_follow_team(img));
        check_symmetry(rec, h3, 3, VERSUS, __LINE__);
        check_symmetry(rec, h3, 3, TOGETHER, __LINE__);
    }
    {
        const Rec rec[4] = { { 3, 0, 0, 0 }, { 0, 0, 30, 0 }, { 1, 1, 0, 0 }, { 2, 0, 0, 0 } };   // 3 humans and a COM on team 0: unbalanced
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        CHECK(roster_apply_image(img, h3, 3, VERSUS, 0) == Result::Ok && balanced(img));
        // anchor = member 1's team (1): members 1, 3 there, member 2 with the COM on team 0
        CHECK(team_of(img, 2) == 1 && team_of(img, 0) == 1 && team_of(img, 3) == 0 && team_of(img, 1) == 0);
        CHECK(team_support(img, 1) == 0xC8 && team_support(img, 0) == 0xD0 && supports_follow_team(img));
        check_symmetry(rec, h3, 3, VERSUS, __LINE__);
        make_room(img, rec, 0x133, 0x133, 1);
        CHECK(roster_apply_image(img, h3, 3, TOGETHER, 0) == Result::Ok && balanced(img));
        CHECK(team_of(img, 2) == 1 && team_of(img, 3) == 1 && team_of(img, 0) == 0 && team_of(img, 1) == 0);   // members 1, 2 together
        CHECK(team_support(img, 1) == 0xC8 && team_support(img, 0) == 0xD1);
        check_symmetry(rec, h3, 3, TOGETHER, __LINE__);
    }
    // N = 4: 2 + 2 in both modes; no COMs
    const HumanPick h4[4] = { { 4, pk(40, 0xD4) }, { 2, pk(20, 0xD2) }, { 1, pk(10, 0xD1) }, { 3, pk(30, 0xD3) } };
    {
        const Rec rec[4] = { { 1, 0, 0, 0 }, { 2, 0, 0, 0 }, { 3, 1, 0, 0 }, { 4, 1, 0, 0 } };
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        ApplyInfo ai;
        CHECK(roster_apply_image(img, h4, 4, VERSUS, 0, &ai) == Result::Ok && ai.moved == 0);
        CHECK(fighter_of(img, 0) == 10 && fighter_of(img, 1) == 20 && fighter_of(img, 2) == 30 && fighter_of(img, 3) == 40);
        CHECK(team_support(img, 0) == 0xD1 && team_support(img, 1) == 0xD3 && supports_follow_team(img));
        check_symmetry(rec, h4, 4, VERSUS, __LINE__);
        check_symmetry(rec, h4, 4, TOGETHER, __LINE__);
    }
    {
        const Rec rec[4] = { { 1, 0, 0, 0 }, { 2, 0, 0, 0 }, { 3, 0, 0, 0 }, { 4, 1, 0, 0 } };   // 3 + 1: rebuilt A, B, A, B
        uint8_t img[kRoomSize]; make_room(img, rec, 0x133, 0x133, 1);
        CHECK(roster_apply_image(img, h4, 4, VERSUS, 0) == Result::Ok && balanced(img));
        CHECK(team_of(img, 0) == 0 && team_of(img, 1) == 1 && team_of(img, 2) == 0 && team_of(img, 3) == 1);
        check_symmetry(rec, h4, 4, VERSUS, __LINE__);
        check_symmetry(rec, h4, 4, TOGETHER, __LINE__);
    }
}

static void test_random()
{
    const uint16_t list[] = { 0, 3, 7, 12, 40 };
    CHECK(resolve_random(5, 1, 0, list, 5) == 5);                 // not Random: unchanged
    CHECK(resolve_random(kRandom, 1, 0, list, 0) == kRandom);     // no list: stays Random (the host's broadcast value stands)
    int hits[5] = {};
    for (uint64_t s = 1; s < 2000; s++) {
        const uint16_t a = resolve_random(kRandom, s * 0x9E3779B97F4A7C15ull, 1, list, 5);
        CHECK(a == resolve_random(kRandom, s * 0x9E3779B97F4A7C15ull, 1, list, 5));   // the same on every PC
        for (int i = 0; i < 5; i++) if (list[i] == a) hits[i]++;
    }
    for (int i = 0; i < 5; i++) CHECK(hits[i] > 250);   // spread over the list
    CHECK(fnv1a64(nullptr, 0) == 0xcbf29ce484222325ull);
    const uint8_t a[1] = { 'a' }; CHECK(fnv1a64(a, 1) == 0xaf63dc4c8601ec8cull);
}

int main()
{
    test_wire();
    test_states();
    test_stale();
    int bad[5] = {};
    for (int n = 2; n <= 4; n++) for (uint32_t s = 1; s <= 400; s++) if (!random_run(s * 7919u + (uint32_t)n, n)) { if (bad[n]++ < 3) std::printf("FAIL random run n=%d seed %u\n", n, s); fails++; }
    test_roster_two();
    test_roster_more();
    test_random();
    if (fails) { std::printf("net pick: %d failure(s)\n", fails); return 1; }
    std::printf("net pick: wire, agreement (2..4 players, 1200 random runs), roster (versus / together, 2..4 players, symmetry) ok\n");
    return 0;
}
