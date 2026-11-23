// jsp_net_pick.h -- ENHANCEMENT (experimental rollback netcode): the character pick after "Select character" in the post-game prompt
// (jsp_net_postgame.h decides Fight again / Select character / Leave; this header carries what each player then picks, and the roster
// write that applies the picks). Built for N = 2..4 human players, one per PC (N = 2 is what the game's Friendly Match gives today).
// Header only, no Windows or game calls, so installer/tools/net_pick_test.cpp runs it on the host. Three parts:
//
// 1. The pick message (link tag kTag 'C'; the net code puts the tag byte in front, like the 'P' post-game message). kMsgLen = 18 bytes:
//      [0] kMagic 0xC5  [1] kVersion 1  [2..5] session tag (u32 LE, session_tag() of the link's plan session id, never 0)
//      [6..7] seq (u16 LE, the post-game seq = the match number that ended)  [8..9] rev (u16 LE)  [10..11] fighter id (u16 LE)
//      [12..13] support id (u16 LE)  [14] flags: bit 0 locked, bit 1 in room, bit 2 team mode Together (0 = Versus), bits 3-7 zero
//      [15] sender's player index 0..3 (0 = the room owner / host), bits 2-7 zero  [16] saw: bit i = I hold player i's locked pick
//      (bits 4-7 zero)  [17] check: XOR of bytes 0..16 with 0x3C.
//    decode() refuses a wrong length, magic, version, reserved bit, check byte or a zero session tag. A message of another link
//    (session tag) or of another post-game (seq) is stale and never changes the state of the round.
//
// 2. PickAgreement, one per PC: IDLE -> start() -> PICKING -> lock() (or the idle auto-lock) -> LOCKED -> READY. READY = this side holds
//    the locked pick of every one of the N players AND every other player's newest message says it holds all N too (its saw mask is
//    full). Every message is the sender's whole state, rev orders a sender's messages, a locked pick never changes (a later locked message
//    with another pick is ignored and counted), and a saw bit is set only after that player's locked pick was taken. So every READY side
//    holds the same N locked picks, whatever the order, loss or duplication of messages. Each player's messages must reach every other
//    player (direct links, or the host forwarding them unchanged); for N = 2 that is the one rollback link.
//    Timeouts: no pick locked within idle_ms (kIdleMs = 120 s; JSP_PICK_IDLE_SECS 30..600) -> the current pick is locked as it is (the
//    start pick: the previous one, or Random); nothing heard from some player for silence_ms (8 s) while picking or locked -> LOST
//    "Connection lost" (the net code then ends the round for everyone, as a LEAVE); locked but not READY idle_ms + wait_ms after start ->
//    LOST. take() resends every resend_ms until stop() (the net code stops it when the next fight runs or the link closes), and for
//    linger_ms after LOST.
//    Team mode: each player's pick carries a wish (Versus / Together); the HOST's wish (player 0) decides (agreed_mode()). The others'
//    wishes are only shown ("Host chose Together"). Rule picked because it is one source, needs no extra round, and is the same on every PC.
//
// 3. roster_apply(): the roster write at the battle builder's entry (0x002EEE90), the same code on every PC. Input: the room's 4 fighter
//    records and team fields as the host's broadcast left them (equal on all PCs), the N human picks with their room member ids, and the
//    agreed team mode. Output: the 4 records and the 2 team supports. A fight is always 2 against 2 (4 records, empty ones COM).
//      - The records with the COM flag (+0x1A) 0 are the humans: there must be exactly N, and their member ids must be exactly the picks'
//        member ids (distinct, non-zero). Anything else is refused (Result) and the native roster stands.
//      - Humans per team: Versus splits them evenly (N = 2: 1 + 1, N = 3: 2 + 1, N = 4: 2 + 2); Together puts them on one team where they
//        fit (N = 2: 2 + 0, N = 3: 2 + 1, N = 4: 2 + 2). If the records already have that split (and every team byte is 0/1, 2 per team),
//        no team byte changes. Otherwise: the anchor team A = the team of the lowest member id human (0 if its byte is not 0/1), humans in
//        member-id order go Versus: A, B, A, B; Together: A, A, B, B. COMs fill the rest: in record order a COM keeps its team while that
//        team has room, then the rest go to the team with room. A record whose team changes and whose slot team byte R+0x56+i is 0/1 gets
//        the new team there too (a 2 = "not set" byte stays 2; UNVERIFIED U8).
//      - Each human's record +0x0E = its own fighter. Supports are per team (R+0x44 team 0, R+0x48 team 1): a team's support is the
//        support of its lowest member id human (with one human a team: that human's own support; Together with 2 humans: the host's);
//        UNVERIFIED (U1) that the game has no per-player support on a shared team. A team without humans keeps its support. Every
//        record's +0x10 = its team's support (what the game's own broadcast decoder does).
//      - Random (kRandom 0x133) in a pick keeps what the host's broadcast put in that human's record (fighter +0x0E, support +0x10): the
//        host already resolved it and every PC holds the same value. The port's own Random (resolve_random) is applied by the caller
//        before roster_apply when it has an owned-by-all list.
//    Why the bytes are the same on every PC: roster_apply is a pure function of its arguments (no globals, clock, RNG, floating point,
//    unordered containers or host-endian loads; every multi-byte field is composed byte by byte in big-endian guest order). It never
//    asks which PC it runs on: players are told apart by the member ids in the records and the picks, ordered by member id or record
//    index, never by "am I the host"; the order of the picks array does not matter (it is sorted by member id first). The records and
//    team fields it reads are the host's broadcast, equal on all PCs before the write, and the picks are equal once PickAgreement is
//    READY everywhere. The one per-PC output is the optional self-pick write of roster_apply_image (R+0x08 / R+0x0C, this PC's own pick
//    block, which the builder does not read for other players: U6), outside the roster bytes.
//    All room offsets below are UNVERIFIED (U1): read from the lift and the trace dumps, not yet confirmed by a mapping run.
#pragma once
#include <cstddef>
#include <cstdint>

namespace jspk {

inline constexpr uint16_t kRandom = 0x133;                     // the game's "Random" id for fighter and support picks
inline constexpr uint16_t kHostMember = 1, kJoinMember = 2;    // room member ids of the owner and the first joiner (U5: always 1 / 2?)
inline constexpr int kMaxPlayers = 4;

enum TeamMode : uint8_t { VERSUS = 0, TOGETHER = 1 };

// ---- 1. the message ----
inline constexpr uint8_t kTag = 'C';
inline constexpr uint8_t kMagic = 0xC5, kVersion = 1;
inline constexpr size_t kMsgLen = 18;
inline constexpr uint8_t kFlagLocked = 1, kFlagInRoom = 2, kFlagTogether = 4;

struct Pick {
    uint16_t fighter = kRandom, support = kRandom;
    TeamMode mode = VERSUS;   // this player's wish; only the host's counts (agreed_mode)
    bool operator==(const Pick&) const = default;
};
struct Msg {
    uint32_t session = 0; uint16_t seq = 0, rev = 0; Pick pick;
    bool locked = false, in_room = false;
    uint8_t player = 0, saw = 0;
};

// The link's 64-bit plan session id folded to the 32-bit tag the message carries (never 0).
inline uint32_t session_tag(uint64_t sid)
{
    const uint32_t t = (uint32_t)sid ^ (uint32_t)(sid >> 32);
    return t ? t : 1u;
}

inline void encode(const Msg& m, uint8_t out[kMsgLen])
{
    out[0] = kMagic; out[1] = kVersion;
    for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t)(m.session >> (8 * i));
    out[6] = (uint8_t)m.seq; out[7] = (uint8_t)(m.seq >> 8);
    out[8] = (uint8_t)m.rev; out[9] = (uint8_t)(m.rev >> 8);
    out[10] = (uint8_t)m.pick.fighter; out[11] = (uint8_t)(m.pick.fighter >> 8);
    out[12] = (uint8_t)m.pick.support; out[13] = (uint8_t)(m.pick.support >> 8);
    out[14] = (uint8_t)((m.locked ? kFlagLocked : 0) | (m.in_room ? kFlagInRoom : 0) | (m.pick.mode == TOGETHER ? kFlagTogether : 0));
    out[15] = (uint8_t)(m.player & 3);
    out[16] = (uint8_t)(m.saw & 0x0F);
    uint8_t x = 0x3C; for (size_t i = 0; i < kMsgLen - 1; i++) x ^= out[i];
    out[17] = x;
}
inline bool decode(const uint8_t* p, size_t n, Msg* m)
{
    if (!p || !m || n != kMsgLen || p[0] != kMagic || p[1] != kVersion || (p[14] & 0xF8) || (p[15] & 0xFC) || (p[16] & 0xF0)) return false;
    uint8_t x = 0x3C; for (size_t i = 0; i < kMsgLen - 1; i++) x ^= p[i];
    if (x != p[17]) return false;
    Msg r;
    r.session = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
    if (!r.session) return false;
    r.seq = (uint16_t)(p[6] | p[7] << 8); r.rev = (uint16_t)(p[8] | p[9] << 8);
    r.pick.fighter = (uint16_t)(p[10] | p[11] << 8); r.pick.support = (uint16_t)(p[12] | p[13] << 8);
    r.pick.mode = (p[14] & kFlagTogether) ? TOGETHER : VERSUS;
    r.locked = (p[14] & kFlagLocked) != 0; r.in_room = (p[14] & kFlagInRoom) != 0;
    r.player = p[15]; r.saw = p[16];
    *m = r;
    return true;
}

// ---- 2. the agreement ----
inline constexpr uint32_t kIdleMs = 120000;     // no pick locked for 2 minutes: the current pick is locked as it is (JSP_PICK_IDLE_SECS)
inline constexpr uint32_t kSilenceMs = 8000;    // nothing heard from a player for 8 s: the link is dead
inline constexpr uint32_t kWaitMs = 60000;      // locked but not READY idle_ms + this after start: a player is gone
inline constexpr uint32_t kResendMs = 250;
inline constexpr uint32_t kLingerMs = 2000;
inline constexpr const char* kIdleKey = "JSP_PICK_IDLE_SECS";

// JSP_PICK_IDLE_SECS: a whole number of seconds 30..600; anything else (unset, empty, junk, out of range) = the default.
inline uint32_t idle_ms_from(const char* v)
{
    if (!v || !*v) return kIdleMs;
    uint32_t s = 0;
    for (const char* c = v; *c; c++) { if (*c < '0' || *c > '9' || s > 100000) return kIdleMs; s = s * 10 + (uint32_t)(*c - '0'); }
    return s >= 30 && s <= 600 ? s * 1000 : kIdleMs;
}

// The agreed team mode: the host's (player 0) wish.
inline TeamMode agreed_mode(const Pick* by_player) { return by_player[0].mode; }

enum State : uint8_t { IDLE = 0, PICKING = 1, LOCKED = 2, READY = 3, LOST = 4 };
enum Reason : uint8_t { R_NONE = 0, R_LINK_LOST = 1, R_PEER_TIMEOUT = 2 };

struct Config {
    uint32_t idle_ms = kIdleMs, silence_ms = kSilenceMs, wait_ms = kWaitMs, resend_ms = kResendMs, linger_ms = kLingerMs;
};

class PickAgreement {
public:
    // A new pick round for post-game seq on the link with this session tag. me = this PC's player index (0 = host), n = players 2..4.
    // initial = the pick locked by the idle timeout when the player locks nothing (the previous pick, or Random). Messages kept aside
    // for (session, seq) are taken now. false: bad me / n.
    bool start(uint32_t session, uint16_t seq, int me, int n, Pick initial, uint64_t now, const Config& c = Config())
    {
        if (n < 2 || n > kMaxPlayers || me < 0 || me >= n) { state_ = IDLE; return false; }
        cfg_ = c; session_ = session; seq_ = seq; me_ = me; n_ = n; state_ = PICKING; reason_ = R_NONE;
        auto_locked_ = false; conflicts_ = 0; rev_ = 1; t0_ = now; lost_at_ = 0; dirty_ = true; sent_ = false; last_sent_ = 0;
        locked_before_lost_ = false;
        for (int i = 0; i < kMaxPlayers; i++) {
            pick_[i] = Pick(); locked_[i] = false; saw_[i] = 0; in_room_[i] = false; peer_rev_[i] = 0; heard_[i] = now;
        }
        pick_[me] = initial;
        Msg keep[kMaxPlayers]; int nk = 0;
        for (int i = 0; i < kMaxPlayers; i++) if (kept_ok_[i] && kept_[i].session == session && kept_[i].seq == seq) keep[nk++] = kept_[i];
        for (int i = 0; i < kMaxPlayers; i++) kept_ok_[i] = false;
        for (int i = 0; i < nk; i++) receive(keep[i], now);
        return true;
    }
    void stop() { state_ = IDLE; }
    State state() const { return state_; }
    Reason reason() const { return reason_; }
    uint16_t seq() const { return seq_; }
    int me() const { return me_; }
    int players() const { return n_; }

    // The local player's current (not yet locked) pick. false: not picking.
    bool set_pick(Pick p)
    {
        if (state_ != PICKING) return false;
        if (p == pick_[me_]) return true;
        pick_[me_] = p; bump();
        return true;
    }
    // Lock the current pick (final). false: not picking.
    bool lock()
    {
        if (state_ != PICKING) return false;
        state_ = LOCKED; locked_[me_] = true; bump();
        try_ready();
        return true;
    }
    // This side reached the room page (the host's start waits for everyone's flag). Sent with the pick.
    void set_in_room(bool v)
    {
        if (state_ == IDLE || state_ == LOST || v == in_room_[me_]) return;
        in_room_[me_] = v; bump();
    }

    // A message from another player. true when it belongs to this round (it proves that player is alive even when it changes nothing).
    bool receive(const Msg& m, uint64_t now)
    {
        if (state_ == IDLE || m.session != session_ || m.seq != seq_) { keep_aside(m); return false; }
        const int p = m.player;
        if (p >= n_ || p == me_) return false;   // not a player of this round, or an echo of this side
        heard_[p] = now;
        if (state_ == LOST) return true;
        if (m.rev <= peer_rev_[p]) return true;   // duplicate, or older than what was taken
        peer_rev_[p] = m.rev;
        in_room_[p] = m.in_room;
        saw_[p] = (uint8_t)(m.saw & full());
        if (!locked_[p]) {
            pick_[p] = m.pick;
            if (m.locked) { locked_[p] = true; bump(); }   // this side's saw bit for p turns on
        } else if (m.locked && !(m.pick == pick_[p])) conflicts_++;   // a locked pick never changes: the first one stands
        try_ready();
        return true;
    }
    bool receive_bytes(const uint8_t* d, size_t n, uint64_t now) { Msg m; return decode(d, n, &m) && receive(m, now); }

    void tick(uint64_t now)
    {
        if (state_ != PICKING && state_ != LOCKED) return;
        for (int i = 0; i < n_; i++) if (i != me_ && now - heard_[i] >= cfg_.silence_ms) { lose(R_LINK_LOST, now); return; }
        if (state_ == PICKING && now - t0_ >= cfg_.idle_ms) { auto_locked_ = true; lock(); return; }
        if (state_ == LOCKED && now - t0_ >= (uint64_t)cfg_.idle_ms + cfg_.wait_ms) lose(R_PEER_TIMEOUT, now);
    }
    // The message to send now, if any: at once after a change, else every resend_ms, until stop(); for linger_ms after LOST.
    bool take(uint64_t now, Msg* out)
    {
        if (state_ == IDLE) return false;
        if (state_ == LOST && now - lost_at_ >= cfg_.linger_ms) return false;
        if (!dirty_ && sent_ && now < last_sent_ + cfg_.resend_ms) return false;
        dirty_ = false; sent_ = true; last_sent_ = now;
        *out = current();
        return true;
    }
    Msg current() const
    {
        Msg m; m.session = session_; m.seq = seq_; m.rev = rev_; m.pick = pick_[me_];
        m.locked = locked_[me_]; m.in_room = in_room_[me_]; m.player = (uint8_t)me_; m.saw = mask();
        return m;
    }

    Pick mine() const { return pick_[me_]; }
    Pick pick_of(int p) const { return p >= 0 && p < kMaxPlayers ? pick_[p] : Pick(); }
    bool locked_of(int p) const { return p >= 0 && p < n_ && locked_[p]; }
    bool in_room_of(int p) const { return p >= 0 && p < n_ && in_room_[p]; }
    bool all_in_room() const { for (int i = 0; i < n_; i++) if (!in_room_[i]) return false; return n_ > 0; }
    int locked_count() const { int c = 0; for (int i = 0; i < n_; i++) c += locked_[i] ? 1 : 0; return c; }
    bool auto_locked() const { return auto_locked_; }
    uint32_t conflicts() const { return conflicts_; }
    // The agreed picks by player index (out[0..n-1]), only when READY (the same on every PC).
    bool picks(Pick out[kMaxPlayers]) const
    {
        if (state_ != READY) return false;
        for (int i = 0; i < kMaxPlayers; i++) out[i] = i < n_ ? pick_[i] : Pick();
        return true;
    }
    // Seconds until the idle auto-lock while picking, else -1.
    int seconds_left(uint64_t now) const
    {
        if (state_ != PICKING) return -1;
        const uint64_t el = now - t0_;
        return el >= cfg_.idle_ms ? 0 : (int)((cfg_.idle_ms - el + 999) / 1000);
    }
    // The picker's status line (printable ASCII).
    const char* status() const
    {
        switch (state_) {
        case PICKING: return locked_count() == n_ - 1 ? (n_ == 2 ? "Opponent is ready" : "Everyone else is ready") : "Choose your fighter";
        case LOCKED: return n_ == 2 ? "Waiting for opponent..." : "Waiting for the other players...";
        case READY: return "Everyone is ready";
        case LOST: return reason_ == R_PEER_TIMEOUT ? "A player did not pick: back to the lobby" : "The other player left or the connection was lost: back to the lobby";
        default: return "";
        }
    }

private:
    uint8_t full() const { return (uint8_t)((1u << n_) - 1u); }
    uint8_t mask() const { uint8_t b = 0; for (int i = 0; i < n_; i++) if (locked_[i]) b |= (uint8_t)(1u << i); return b; }
    void bump() { rev_++; dirty_ = true; }
    void try_ready()
    {
        if (state_ != LOCKED || mask() != full()) return;
        for (int i = 0; i < n_; i++) if (i != me_ && saw_[i] != full()) return;
        state_ = READY;
    }
    void lose(Reason r, uint64_t now) { locked_before_lost_ = state_ == LOCKED; state_ = LOST; reason_ = r; lost_at_ = now; dirty_ = true; }
    void keep_aside(const Msg& m)   // the newest message per player of one later round (that player reached its picker first)
    {
        const int p = m.player & 3;
        if (kept_ok_[p] && kept_[p].session == m.session && kept_[p].seq == m.seq && m.rev <= kept_[p].rev) return;
        kept_[p] = m; kept_ok_[p] = true;
    }

    Config cfg_;
    State state_ = IDLE;
    Reason reason_ = R_NONE;
    uint32_t session_ = 0, conflicts_ = 0;
    uint16_t seq_ = 0, rev_ = 1;
    int me_ = 0, n_ = 0;
    Pick pick_[kMaxPlayers];
    bool locked_[kMaxPlayers] = {}, in_room_[kMaxPlayers] = {};
    uint8_t saw_[kMaxPlayers] = {};
    uint16_t peer_rev_[kMaxPlayers] = {};
    uint64_t heard_[kMaxPlayers] = {};
    bool auto_locked_ = false, locked_before_lost_ = false;
    uint64_t t0_ = 0, lost_at_ = 0, last_sent_ = 0;
    bool dirty_ = false, sent_ = false;
    Msg kept_[kMaxPlayers];
    bool kept_ok_[kMaxPlayers] = {};
};

// ---- Random, resolved by the port from shared inputs only ----
inline uint64_t fnv1a64(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
// pick unchanged unless it is kRandom and the list is not empty; else list[FNV-1a(session id LE, slot) mod n]. session_id = the match
// session id of the coming match (jspnet::match_session_id(plan, k + 1)); slot = 2 * player index (+1 for the support); list = the ids
// every player owns, in the stock list's order (the same order on every PC).
inline uint16_t resolve_random(uint16_t pick, uint64_t session_id, uint8_t slot, const uint16_t* list, size_t n)
{
    if (pick != kRandom || !list || !n) return pick;
    uint8_t b[9];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(session_id >> (8 * i));
    b[8] = slot;
    return list[fnv1a64(b, sizeof b) % n];
}

// ---- 3. the roster write ----
// Room data R (0x14C bytes, big-endian guest memory). UNVERIFIED (U1): from the reset 0x0029F78C, the decoder 0x0044E794 and the dumps.
inline constexpr size_t kRoomSize = 0x14C;
inline constexpr uint32_t kOffSelfFighter = 0x08;    // u32: this PC's own Battle Character pick (UNVERIFIED U1)
inline constexpr uint32_t kOffSelfSupport = 0x0C;    // u32: this PC's own Support pick (UNVERIFIED U1)
inline constexpr uint32_t kOffTeamSupport0 = 0x44;   // u32: support of team 0 (UNVERIFIED U1)
inline constexpr uint32_t kOffTeamSupport1 = 0x48;   // u32: support of team 1 (UNVERIFIED U1)
inline constexpr uint32_t kOffSlotTeam = 0x56;       // u8 x4: per-slot team bytes, 2 = not set (UNVERIFIED U1, U8)
inline constexpr uint32_t kOffRecords = 0x68;        // 4 fighter records of kRecSize (UNVERIFIED U1)
inline constexpr uint32_t kRecSize = 0x24;
inline constexpr uint32_t kRecMember = 0x08;         // u16: room member id, 0 = COM (UNVERIFIED U1/U5)
inline constexpr uint32_t kRecFighter = 0x0E;        // u16: battle character id (UNVERIFIED U1: id or index)
inline constexpr uint32_t kRecSupport = 0x10;        // u16: support = its team's value (UNVERIFIED U1: units)
inline constexpr uint32_t kRecTeam = 0x16;           // u8: team 0 / 1, 2 = unset (UNVERIFIED U1)
inline constexpr uint32_t kRecCom = 0x1A;            // u8: non-zero = COM (UNVERIFIED U1)
static_assert(kOffRecords + 4 * kRecSize <= kRoomSize, "records inside the room data");

inline uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

// One fighter record, as raw guest bytes (every field not named here is carried unchanged).
struct Record {
    uint8_t raw[kRecSize] = {};
    uint16_t member() const { return be16(raw + kRecMember); }
    uint16_t fighter() const { return be16(raw + kRecFighter); }
    uint16_t support() const { return be16(raw + kRecSupport); }
    uint8_t team() const { return raw[kRecTeam]; }
    bool com() const { return raw[kRecCom] != 0; }
    void set_fighter(uint16_t v) { put_be16(raw + kRecFighter, v); }
    void set_support(uint16_t v) { put_be16(raw + kRecSupport, v); }
    void set_team(uint8_t v) { raw[kRecTeam] = v; }
};
// The part of R the roster write reads and writes.
struct Roster {
    Record rec[4];
    uint32_t team_support[2] = { kRandom, kRandom };
    uint8_t slot_team[4] = { 2, 2, 2, 2 };
};
// One human player's agreed pick with its room member id.
struct HumanPick { uint16_t member = 0; Pick pick; };

inline Roster roster_read(const uint8_t* img)
{
    Roster r;
    for (uint32_t i = 0; i < 4; i++)
        for (uint32_t b = 0; b < kRecSize; b++) r.rec[i].raw[b] = img[kOffRecords + kRecSize * i + b];
    r.team_support[0] = be32(img + kOffTeamSupport0); r.team_support[1] = be32(img + kOffTeamSupport1);
    for (uint32_t i = 0; i < 4; i++) r.slot_team[i] = img[kOffSlotTeam + i];
    return r;
}
inline void roster_write(const Roster& r, uint8_t* img)
{
    for (uint32_t i = 0; i < 4; i++)
        for (uint32_t b = 0; b < kRecSize; b++) img[kOffRecords + kRecSize * i + b] = r.rec[i].raw[b];
    put_be32(img + kOffTeamSupport0, r.team_support[0]); put_be32(img + kOffTeamSupport1, r.team_support[1]);
    for (uint32_t i = 0; i < 4; i++) img[kOffSlotTeam + i] = r.slot_team[i];
}

enum class Result : uint8_t { Ok = 0, BadCount, BadPicks, HumansMismatch };
inline const char* result_text(Result r)
{
    switch (r) {
    case Result::Ok: return "ok";
    case Result::BadCount: return "not 2..4 human picks";
    case Result::BadPicks: return "the picks' member ids are zero or repeated";
    case Result::HumansMismatch: return "the human records are not exactly the picks' members";
    }
    return "?";
}
struct ApplyInfo {
    int human_rec[kMaxPlayers] = { -1, -1, -1, -1 };   // record index of each human, in member-id order
    int moved = 0;                                       // records whose team changed
};

// How many humans go on the anchor team / the other team.
inline void human_split(int n, TeamMode mode, int* on_a, int* on_b)
{
    *on_a = mode == TOGETHER ? (n < 2 ? n : 2) : (n + 1) / 2;
    *on_b = n - *on_a;
}

// The pure roster write (see the top of the file). picks[0..n-1] in any order. On refusal *out = in.
inline Result roster_apply(const Roster& in, const HumanPick* picks, int n, TeamMode mode, Roster* out, ApplyInfo* info = nullptr)
{
    *out = in;
    ApplyInfo ai;
    if (info) *info = ai;
    if (!picks || n < 2 || n > kMaxPlayers) return Result::BadCount;
    HumanPick hp[kMaxPlayers];
    for (int i = 0; i < n; i++) hp[i] = picks[i];
    for (int i = 1; i < n; i++)   // member-id order (insertion sort: the caller's order never matters)
        for (int j = i; j > 0 && hp[j].member < hp[j - 1].member; j--) { const HumanPick t = hp[j]; hp[j] = hp[j - 1]; hp[j - 1] = t; }
    for (int i = 0; i < n; i++) if (!hp[i].member || (i && hp[i].member == hp[i - 1].member)) return Result::BadPicks;
    int nh = 0;
    for (int r = 0; r < 4; r++) {
        if (in.rec[r].com()) continue;
        nh++;
        int k = -1;
        for (int i = 0; i < n; i++) if (hp[i].member == in.rec[r].member()) k = i;
        if (k < 0 || ai.human_rec[k] >= 0) return Result::HumansMismatch;
        ai.human_rec[k] = r;
    }
    if (nh != n) return Result::HumansMismatch;

    Roster r = in;
    // teams: keep them when they already give the mode's split, else rebuild from the anchor team
    int on_a = 0, on_b = 0;
    human_split(n, mode, &on_a, &on_b);
    int hum_team[2] = { 0, 0 }, all_team[2] = { 0, 0 };
    bool bytes_ok = true;
    for (int i = 0; i < 4; i++) { const uint8_t t = in.rec[i].team(); if (t > 1) bytes_ok = false; else { all_team[t]++; if (!in.rec[i].com()) hum_team[t]++; } }
    const bool split_ok = bytes_ok && all_team[0] == 2 && all_team[1] == 2 &&
                          ((hum_team[0] == on_a && hum_team[1] == on_b) || (hum_team[1] == on_a && hum_team[0] == on_b));
    if (!split_ok) {
        const uint8_t t0 = in.rec[ai.human_rec[0]].team();
        const uint8_t A = t0 <= 1 ? t0 : 0, B = (uint8_t)(A ^ 1);
        int room[2] = { 2, 2 };
        for (int i = 0; i < n; i++) {
            const uint8_t t = mode == TOGETHER ? (i < on_a ? A : B) : (i % 2 == 0 ? A : B);
            r.rec[ai.human_rec[i]].set_team(t); room[t]--;
        }
        bool placed[4] = {};
        for (int i = 0; i < 4; i++) {   // a COM keeps its team while there is room
            if (!in.rec[i].com()) continue;
            const uint8_t t = in.rec[i].team();
            if (t <= 1 && room[t] > 0) { r.rec[i].set_team(t); room[t]--; placed[i] = true; }
        }
        for (int i = 0; i < 4; i++) {   // the rest go where there is room
            if (!in.rec[i].com() || placed[i]) continue;
            const uint8_t t = room[0] > 0 ? 0 : 1;
            r.rec[i].set_team(t); room[t]--;
        }
        for (int i = 0; i < 4; i++) {
            if (r.rec[i].team() == in.rec[i].team()) continue;
            ai.moved++;
            if (in.slot_team[i] <= 1) r.slot_team[i] = r.rec[i].team();
        }
    }
    // fighters: the own pick, or (Random) what the host's broadcast put in the record
    for (int i = 0; i < n; i++) if (hp[i].pick.fighter != kRandom) r.rec[ai.human_rec[i]].set_fighter(hp[i].pick.fighter);
    // supports: per team; the lowest member id human on a team gives its support (picks are in member-id order: the first one wins)
    bool set[2] = { false, false };
    for (int i = 0; i < n; i++) {
        const Record& rec = r.rec[ai.human_rec[i]];
        const uint8_t t = rec.team();
        if (set[t]) continue;
        set[t] = true;
        r.team_support[t] = hp[i].pick.support != kRandom ? hp[i].pick.support : in.rec[ai.human_rec[i]].support();
    }
    for (int i = 0; i < 4; i++) r.rec[i].set_support((uint16_t)r.team_support[r.rec[i].team()]);
    *out = r;
    if (info) *info = ai;
    return Result::Ok;
}

// The same on a whole R image (what the builder hook copies out of guest memory and back). self_member: 0 = leave R+0x08 / R+0x0C
// alone; else this PC's member id: its own pick block gets its final fighter and its team's support (the one per-PC write).
inline Result roster_apply_image(uint8_t img[kRoomSize], const HumanPick* picks, int n, TeamMode mode, uint16_t self_member, ApplyInfo* info = nullptr)
{
    Roster out; ApplyInfo ai;
    const Result res = roster_apply(roster_read(img), picks, n, mode, &out, &ai);
    if (info) *info = ai;
    if (res != Result::Ok) return res;
    roster_write(out, img);
    if (self_member) {
        for (int r = 0; r < 4; r++) {
            if (out.rec[r].com() || out.rec[r].member() != self_member) continue;
            put_be32(img + kOffSelfFighter, out.rec[r].fighter());
            put_be32(img + kOffSelfSupport, out.team_support[out.rec[r].team()]);
        }
    }
    return res;
}

}  // namespace jspk
