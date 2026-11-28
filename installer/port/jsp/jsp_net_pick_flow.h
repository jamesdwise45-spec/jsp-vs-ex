// jsp_net_pick_flow.h -- ENHANCEMENT (experimental rollback netcode; JSP_ROLLBACK_PICK=1, default 0 until a mapping run proves the game side): the
// wiring of the after-match prompt (jsp_net_postgame.h: Fight again / Select character / Leave) and the character pick (jsp_net_pick.h) on one
// rollback link of 2..4 players, as pure code (no Windows, no game memory), so installer/tools/net_pick_flow_test.cpp runs N PCs of it on the host.
// jsp_net_game.cpp holds one Flow per link under the pump mutex, routes the link's 'P' / 'C' datagrams into it, sends what it hands out, and acts
// on what poll() returns; jsp_net_native.cpp applies the agreed roster at the battle builder's entry (apply_pair below).
//
// Five parts:
//   1. GroupAgreement: the post-game choice for 3..4 players (the 2-player choice stays jspg::Agreement, byte for byte). A choice is final when it is
//      made; every message carries the sender's whole state and a saw mask (bit i = I hold player i's choice). A LEAVE decides at once (that PC
//      leaves, a PC that hears it leaves the round with "A player left"); otherwise a PC decides once it holds all N choices and every other
//      player's newest message has a full saw mask: Fight again only if everyone chose it, else Select character (jspg::combine over all N).
//      Same argument as jsp_net_pick.h: every deciding PC holds the same N final choices. Wire (kGroupMsgLen = 14 bytes, tag 'P' like the
//      2-player message, told apart by its length and magic): [0] 0xB8 [1] version 1 [2..5] session tag (u32 LE, jspk::session_tag of the
//      link's plan session id, never 0: an older link's message is refused) [6..7] seq u16 LE [8..9] rev u16 LE [10] choice (bits 0-1, bits 2-7
//      zero) [11] player index 0..3 (bits 2-7 zero) [12] saw (bits 4-7 zero) [13] check: XOR of bytes 0..12 with 0x5A.
//      A player that leaves after the match ends the round for the others too (their link closes WITHOUT giving the room up): the native room
//      pairing re-plans the link for the members that stay once the leaver is out of the room (jsp4::replan), so "a LEAVE removes only that
//      player" holds at the room level, not inside one link.
//   2. Postgame: jspg::Agreement for 2 players, GroupAgreement for 3..4, behind one interface.
//   3. Flow: one per PC and link. open() when the plan is known (player index = peer id, 0 = the host; battle slot of every player), route() every
//      datagram of the link's settings channel, pump() every pump tick, match_over() at a normal match end, poll() on the main thread. After a
//      Fight again AND after a Select character a pick round (jspk::PickAgreement) runs: for Fight again every player's last pick is locked at
//      once (the round only carries the "in the room" flags the host's start waits for); for Select character each player picks on its own PC.
//      hold() says when the native re-arm of the next match waits. pair() is what the builder applies: the last committed pair (a round is
//      committed when it becomes READY on this PC), else nothing (native roster). The SAME on every PC at the build because the fight cannot
//      start before every PC is READY: a PC sends "in the room" only once its round is READY, the host's start waits for every player's flag
//      (start_ok), and jsp_net_game.cpp swallows the host's own Cross on the room page while a round runs and start_ok is false (block_start).
//      route() takes the sender's peer id: a message whose player index is not its sender's is refused (counted in spoofed()).
//      FIX (pick7, the freeze owner while the menu is up): the frame clock's 60 s failure (jsp_stall_rule.h kFailMs) is watched only while a
//      match runs, and between matches the pump thread kept answering for a game that no longer ran: the prompt's datagrams, the hellos and
//      the pick messages went on, so the other PC waited in its prompt for the idle timeout, and a host in a READY round with a frozen
//      joiner never got its start (start_ok) and kept its Cross blocked. Now the pump thread feeds game_alive() with every new flip; with the
//      menu up and no flip for kFreezeMs (= kFailMs) the pump thread leaves for this PC: Leave when no choice was made, no more pick
//      messages, and "gone" in every hello. The other PC closes its round at once (Act::PeerFroze); this PC's main thread, if it comes
//      back, gets Act::Froze and closes the link too.
//   4. apply_pair(): the roster write on a room image. Player i's room member id is the member id in the record at its battle slot (slot = record
//      index, the 4-player claim; 2 players: host slot 0, joiner slot 1). TODO-MAPPING (U5): if that record is a COM or has member id 0, the human
//      records in record order are given to the players in slot order (fallback, logged); both are functions of R and the plan only.
//      FIX (test13) agreed mode ROOM (the default: the host's pick carries the room-teams bit, JSP_PICK_ROOM_TEAMS): the record team bytes
//      are the players' choice in the room and are KEPT when they are a valid 2 + 2 (jsp_net_pick.h 3.; else the nearest valid layout). They
//      are the host's broadcast, the same bytes the game's own builder places from on every PC; versus_apply (off by default since test13)
//      never runs while a pair is active. Agreed mode VERSUS / TOGETHER (JSP_PICK_ROOM_TEAMS=0 on the host, the versus safeguard on, or the
//      port picker's Together row): the record team bytes are cleared to "unset" before roster_apply, so the teams come only from the member
//      ids, the record order and the agreed mode (anchor team 0), never from team bytes that a PC's own versus_apply may or may not have
//      swapped yet. Either way the fighter map of the session start pairs pads and fighters by member id (jnf::net_ports), so every layout
//      keeps each player's pad on its own fighter (jspo::predict_map / own_ok, host-tested). This PC's own pick
//      block (R+0x08 / +0x0C) is NOT written: it would differ between PCs (TODO-MAPPING U6/U6b: if the loading scene's upsert 0x002EE96C
//      rebuilds this PC's record from it after the builder's entry, JSP_PICK_SCENE_APPLY=1 writes the roster again on every loading update).
//   5. The picker's texts: the lists (named ids only: fighters in the select screen's own order, the stock list 0x00756C64; Supports
//      alphabetically), the names (jsp_unit_names.h), a text cut to a pixel width with "...", and one line per player's pick.
#pragma once
#include "jsp_net_pick.h"
#include "jsp_net_postgame.h"
#include "jsp_unit_names.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace jspf {

inline constexpr uint8_t kTagPostgame = 'P', kTagPick = jspk::kTag;   // jsp_net_session.h kLinkTagPostgame / kLinkTagPick
inline constexpr const char* kKey = "JSP_ROLLBACK_PICK";
// JSP_ROLLBACK_PICK: exactly "1" turns the prompt, the pick and the roster write on; anything else (unset, 0, junk) = off (today's behaviour).
inline bool key_on(const char* v) { return v && v[0] == '1' && !v[1]; }

// ---- 1. the 3..4 player choice ----
inline constexpr uint8_t kGroupMagic = 0xB8, kGroupVersion = 1;
inline constexpr size_t kGroupMsgLen = 14;
struct GroupMsg { uint32_t session = 0; uint16_t seq = 0, rev = 0; jspg::Choice pick = jspg::NONE; uint8_t player = 0, saw = 0; };

inline void group_encode(const GroupMsg& m, uint8_t out[kGroupMsgLen])
{
    out[0] = kGroupMagic; out[1] = kGroupVersion;
    for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t)(m.session >> (8 * i));
    out[6] = (uint8_t)m.seq; out[7] = (uint8_t)(m.seq >> 8);
    out[8] = (uint8_t)m.rev; out[9] = (uint8_t)(m.rev >> 8);
    out[10] = (uint8_t)(m.pick & 3); out[11] = (uint8_t)(m.player & 3); out[12] = (uint8_t)(m.saw & 0x0F);
    uint8_t x = 0x5A; for (size_t i = 0; i < kGroupMsgLen - 1; i++) x ^= out[i];
    out[13] = x;
}
inline bool group_decode(const uint8_t* p, size_t n, GroupMsg* m)
{
    if (!p || !m || n != kGroupMsgLen || p[0] != kGroupMagic || p[1] != kGroupVersion || (p[10] & 0xFC) || (p[11] & 0xFC) || (p[12] & 0xF0)) return false;
    uint8_t x = 0x5A; for (size_t i = 0; i < kGroupMsgLen - 1; i++) x ^= p[i];
    if (x != p[13]) return false;
    const uint32_t s = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
    if (!s) return false;
    m->session = s; m->seq = (uint16_t)(p[6] | p[7] << 8); m->rev = (uint16_t)(p[8] | p[9] << 8);
    m->pick = (jspg::Choice)p[10]; m->player = p[11]; m->saw = p[12];
    return true;
}

class GroupAgreement {
public:
    static constexpr int kMax = jspk::kMaxPlayers;
    // A new post-game for match seq on the link with this session tag; me = this PC's player index, n = players 2..4. Messages kept aside for
    // (session, seq) are taken now. false: bad me / n.
    bool start(uint32_t session, uint16_t seq, int me, int n, uint64_t now, const jspg::Config& c = jspg::Config())
    {
        if (n < 2 || n > kMax || me < 0 || me >= n) { started_ = false; return false; }
        cfg_ = c; session_ = session; seq_ = seq; me_ = me; n_ = n; started_ = true; outcome_ = jspg::PENDING; reason_ = jspg::R_NONE;
        rev_ = 1; t0_ = now; decided_at_ = 0; dirty_ = true; sent_ = false; last_sent_ = 0; conflicts_ = 0;
        for (int i = 0; i < kMax; i++) { choice_[i] = jspg::NONE; saw_[i] = 0; peer_rev_[i] = 0; heard_[i] = now; }
        GroupMsg keep[kMax]; int nk = 0;
        for (int i = 0; i < kMax; i++) if (kept_ok_[i] && kept_[i].session == session && kept_[i].seq == seq) keep[nk++] = kept_[i];
        for (int i = 0; i < kMax; i++) kept_ok_[i] = false;
        for (int i = 0; i < nk; i++) receive(keep[i], now);
        return true;
    }
    void stop() { started_ = false; }
    bool started() const { return started_; }
    uint16_t seq() const { return seq_; }
    int players() const { return n_; }

    // The local player's choice, final at once. Choosing the same again is accepted (no change); another one after a choice is refused.
    bool pick(jspg::Choice c, uint64_t now)
    {
        if (!started_ || c == jspg::NONE) return false;
        if (choice_[me_] != jspg::NONE) return c == choice_[me_];
        if (outcome_ != jspg::PENDING) return false;
        choice_[me_] = c; bump();
        if (c == jspg::LEAVE) { decide(jspg::O_LEAVE, jspg::R_SELF, now); return true; }
        try_decide(now);
        return true;
    }
    // A message of another player. true when it belongs to this round (it proves that player is alive even when it changes nothing).
    bool receive(const GroupMsg& m, uint64_t now)
    {
        if (!started_ || m.session != session_ || m.seq != seq_) { keep_aside(m); return false; }
        const int p = m.player;
        if (p >= n_ || p == me_) return false;
        heard_[p] = now;
        if (outcome_ != jspg::PENDING) return true;
        if (m.rev <= peer_rev_[p]) return true;
        peer_rev_[p] = m.rev;
        saw_[p] = (uint8_t)(m.saw & full());
        if (choice_[p] == jspg::NONE && m.pick != jspg::NONE) {
            choice_[p] = m.pick; bump();   // this side's saw bit for p turns on
            if (m.pick == jspg::LEAVE) { decide(jspg::O_LEAVE, jspg::R_PEER_LEFT, now); return true; }
        } else if (choice_[p] != jspg::NONE && m.pick != jspg::NONE && m.pick != choice_[p]) conflicts_++;   // a choice never changes: the first stands
        try_decide(now);
        return true;
    }
    bool receive_bytes(const uint8_t* d, size_t n, uint64_t now) { GroupMsg m; return group_decode(d, n, &m) && receive(m, now); }

    void tick(uint64_t now)
    {
        if (!started_ || outcome_ != jspg::PENDING) return;
        // FIX (test13 review of the 3-4 player menu, REVIEW-MENU4 1): a silent player ends the round as "link lost" WITHOUT writing a LEAVE into this
        // PC's own choice. Before, that LEAVE went out and every PC that still heard this one took it for a player who chose to leave (R_PEER_LEFT)
        // while this PC had R_LINK_LOST: the PCs split on the leave rule's hold (jsp_net_menu4.h). Now the others reach R_LINK_LOST by their own
        // silence check once this PC's messages stop (linger_ms after its decision). Same wire; the 2-player jspg::Agreement is untouched.
        for (int i = 0; i < n_; i++)
            if (i != me_ && now - heard_[i] >= cfg_.silence_ms) { decide(jspg::O_LEAVE, jspg::R_LINK_LOST, now); return; }
        if (choice_[me_] == jspg::NONE && now - t0_ >= cfg_.idle_ms) { choice_[me_] = jspg::LEAVE; bump(); decide(jspg::O_LEAVE, jspg::R_IDLE, now); return; }
        if (choice_[me_] != jspg::NONE && now - t0_ >= (uint64_t)cfg_.idle_ms + cfg_.silence_ms) decide(jspg::O_LEAVE, jspg::R_LINK_LOST, now);
    }
    bool take(uint64_t now, GroupMsg* out)
    {
        if (!started_) return false;
        if (outcome_ != jspg::PENDING && now - decided_at_ >= cfg_.linger_ms) return false;
        if (!dirty_ && sent_ && now < last_sent_ + cfg_.resend_ms) return false;
        dirty_ = false; sent_ = true; last_sent_ = now;
        *out = current();
        return true;
    }
    GroupMsg current() const { GroupMsg m; m.session = session_; m.seq = seq_; m.rev = rev_; m.pick = choice_[me_]; m.player = (uint8_t)me_; m.saw = mask(); return m; }

    jspg::Choice mine() const { return started_ ? choice_[me_] : jspg::NONE; }
    jspg::Choice choice_of(int p) const { return p >= 0 && p < n_ ? choice_[p] : jspg::NONE; }
    int chosen_others() const { int c = 0; for (int i = 0; i < n_; i++) c += i != me_ && choice_[i] != jspg::NONE; return c; }
    jspg::Outcome outcome() const { return outcome_; }
    jspg::Reason reason() const { return reason_; }
    uint32_t conflicts() const { return conflicts_; }

    jspg::View view(int sel, uint64_t now) const
    {
        jspg::View v;
        v.mine = mine(); v.locked = v.mine != jspg::NONE; v.outcome = outcome_; v.reason = reason_;
        v.sel = v.locked ? (int)v.mine - 1 : (sel < 0 ? 0 : sel >= jspg::kRows ? jspg::kRows - 1 : sel);
        if (outcome_ != jspg::PENDING) { v.status = outcome_text(outcome_, reason_); return v; }
        const int others = n_ - 1, k = chosen_others();
        if (k) v.peer_line = std::to_string(k) + " of " + std::to_string(others) + " other players have chosen";
        v.status = v.locked ? "Waiting for the other players..." : "Choose what to do next";
        if (!v.locked) {
            const uint64_t el = now - t0_;
            const uint64_t left = el >= cfg_.idle_ms ? 0 : cfg_.idle_ms - el;
            if (left <= jspg::kCountdownMs) { v.seconds_left = (int)((left + 999) / 1000); v.countdown = "Returning to lobby in " + std::to_string(v.seconds_left) + " s"; }
        }
        return v;
    }
    static const char* outcome_text(jspg::Outcome o, jspg::Reason r)
    {
        if (o == jspg::O_LEAVE && r == jspg::R_PEER_LEFT) return "A player left: back to the room";
        return jspg::Agreement::outcome_text(o, r);
    }

private:
    uint8_t full() const { return (uint8_t)((1u << n_) - 1u); }
    uint8_t mask() const { uint8_t b = 0; for (int i = 0; i < n_; i++) if (choice_[i] != jspg::NONE) b |= (uint8_t)(1u << i); return b; }
    void bump() { rev_++; dirty_ = true; }
    void decide(jspg::Outcome o, jspg::Reason r, uint64_t now) { outcome_ = o; reason_ = r; decided_at_ = now; dirty_ = true; }
    void try_decide(uint64_t now)
    {
        if (outcome_ != jspg::PENDING || mask() != full()) return;
        for (int i = 0; i < n_; i++) if (i != me_ && saw_[i] != full()) return;
        jspg::Outcome o = jspg::O_REMATCH;
        for (int i = 0; i < n_; i++) if (choice_[i] == jspg::CHARSELECT) o = jspg::O_CHARSELECT;
        decide(o, jspg::R_NONE, now);
    }
    void keep_aside(const GroupMsg& m)   // the newest message per player of one later round (that player reached its prompt first)
    {
        const int p = m.player & 3;
        if (kept_ok_[p] && kept_[p].session == m.session && kept_[p].seq == m.seq && m.rev <= kept_[p].rev) return;
        kept_[p] = m; kept_ok_[p] = true;
    }

    jspg::Config cfg_;
    bool started_ = false;
    uint32_t session_ = 0;
    uint16_t seq_ = 0, rev_ = 1;
    int me_ = 0, n_ = 0;
    jspg::Choice choice_[kMax] = {};
    uint8_t saw_[kMax] = {};
    uint16_t peer_rev_[kMax] = {};
    uint64_t heard_[kMax] = {};
    jspg::Outcome outcome_ = jspg::PENDING;
    jspg::Reason reason_ = jspg::R_NONE;
    uint64_t t0_ = 0, decided_at_ = 0, last_sent_ = 0;
    bool dirty_ = false, sent_ = false;
    uint32_t conflicts_ = 0;
    GroupMsg kept_[kMax];
    bool kept_ok_[kMax] = {};
};

// ---- 1b. FIX (pick7): the switch handshake ----
// JSP_ROLLBACK_PICK is read on each PC and is not one of the session handshake's values. With it on one PC only, that PC opened the prompt with
// the pad captured, never got an answer and sat in it until the idle + silence timeout (about 188 s) while the other PC was back in its room.
// Now every PC with the switch on sends a hello on its link (tag 'P', kHelloLen bytes) every kHelloMs until it agrees, then every kHelloSlowMs:
// heard = the players whose hello it holds; declined = the match (seq + 1) whose prompt it did not open (0 = none). A PC agrees once it holds
// every other player's hello AND every other player's hello says it heard this PC's. The prompt of a match opens only on agreement
// (Flow::match_end); a PC without agreement declines that match and says so in its hellos. A PC that opened its prompt and then hears that
// another player declined the same match closes it at once (Act::PeerOff), so a last hello lost at the very end of a match cannot leave one
// PC in a prompt the other never opened. A PC with the switch off sends nothing (its Flow is closed) and only counts the hellos it drops
// (off_hellos, logged once by jsp_net_game.cpp). Either way both PCs behave as off for that match and the log says why.
// Wire (kHelloLen = 11 bytes): [0] 0xBA [1] version 1 [2..5] session tag (u32 LE, never 0) [6] heard (bits 0-3), bit 4 gone (see 3: the freeze owner), bits 5-7 zero [7] player
// index 0..3 (bits 2-7 zero) [8..9] declined (u16 LE) [10] check: XOR of bytes 0..9 with 0x69. Its length and magic tell it from the
// 2-player choice (8 bytes, 0xB7) and the 3..4 player choice (14 bytes, 0xB8).
inline constexpr uint8_t kHelloMagic = 0xBA, kHelloVersion = 1;
inline constexpr size_t kHelloLen = 11;
inline constexpr uint64_t kHelloMs = 1000, kHelloSlowMs = 3000;
inline constexpr uint64_t kFreezeMs = 60000;   // FIX (pick7): no flip this long with the menu up: the pump thread leaves for this PC (= jsr::kFailMs)
struct Hello { uint32_t session = 0; uint8_t heard = 0, player = 0; uint16_t declined = 0; bool gone = false; };
inline void hello_encode(const Hello& h, uint8_t out[kHelloLen])
{
    out[0] = kHelloMagic; out[1] = kHelloVersion;
    for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t)(h.session >> (8 * i));
    out[6] = (uint8_t)((h.heard & 0x0F) | (h.gone ? 0x10 : 0)); out[7] = (uint8_t)(h.player & 3);
    out[8] = (uint8_t)h.declined; out[9] = (uint8_t)(h.declined >> 8);
    uint8_t x = 0x69; for (size_t i = 0; i < kHelloLen - 1; i++) x ^= out[i];
    out[10] = x;
}
inline bool hello_decode(const uint8_t* p, size_t n, Hello* h)
{
    if (!p || !h || n != kHelloLen || p[0] != kHelloMagic || p[1] != kHelloVersion || (p[6] & 0xE0) || (p[7] & 0xFC)) return false;
    uint8_t x = 0x69; for (size_t i = 0; i < kHelloLen - 1; i++) x ^= p[i];
    if (x != p[10]) return false;
    const uint32_t s = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
    if (!s) return false;
    h->session = s; h->heard = (uint8_t)(p[6] & 0x0F); h->gone = (p[6] & 0x10) != 0; h->player = p[7]; h->declined = (uint16_t)(p[8] | p[9] << 8);
    return true;
}

// ---- 2. one interface for 2 and 3..4 players ----
class Postgame {
public:
    static constexpr size_t kMaxMsg = kGroupMsgLen;   // >= jspg::kMsgLen
    void configure(int me, int n, uint32_t session = 1) { me_ = me; n_ = n; session_ = session; on_ = false; }
    bool start(uint16_t seq, uint64_t now, const jspg::Config& c = jspg::Config())
    {
        if (n_ == 2) { two_.start(seq, now, c); on_ = true; }
        else on_ = group_.start(session_, seq, me_, n_, now, c);
        return on_;
    }
    void stop() { on_ = false; }
    bool on() const { return on_; }
    uint16_t seq() const { return n_ == 2 ? two_.seq() : group_.seq(); }
    bool pick(jspg::Choice c, uint64_t now) { return on_ && (n_ == 2 ? two_.pick(c, now) : group_.pick(c, now)); }
    bool receive_bytes(const uint8_t* p, size_t n, uint64_t now)   // also before start(): kept aside for that seq
    {
        if (n_ == 2) return two_.receive_bytes(p, n, now);
        return n_ > 2 && group_.receive_bytes(p, n, now);
    }
    void link_alive(uint64_t now) { if (on_ && n_ == 2) two_.link_alive(now); }   // 3..4: the silence check is per player (its own messages)
    void tick(uint64_t now) { if (!on_) return; if (n_ == 2) two_.tick(now); else group_.tick(now); }
    // The message to send now (its length, 0 = none) into out[kMaxMsg].
    size_t take(uint64_t now, uint8_t* out)
    {
        if (!on_) return 0;
        if (n_ == 2) { jspg::Msg m; if (!two_.take(now, &m)) return 0; jspg::encode(m, out); return jspg::kMsgLen; }
        GroupMsg m; if (!group_.take(now, &m)) return 0; group_encode(m, out); return kGroupMsgLen;
    }
    jspg::Outcome outcome() const { return !on_ ? jspg::PENDING : n_ == 2 ? two_.outcome() : group_.outcome(); }
    jspg::Reason reason() const { return !on_ ? jspg::R_NONE : n_ == 2 ? two_.reason() : group_.reason(); }
    jspg::Choice mine() const { return !on_ ? jspg::NONE : n_ == 2 ? two_.mine() : group_.mine(); }
    jspg::View view(int sel, uint64_t now) const { return n_ == 2 ? two_.view(sel, now) : group_.view(sel, now); }
    const char* outcome_text() const { return n_ == 2 ? jspg::Agreement::outcome_text(outcome(), reason()) : GroupAgreement::outcome_text(outcome(), reason()); }

private:
    jspg::Agreement two_;
    GroupAgreement group_;
    int me_ = 0, n_ = 0;
    uint32_t session_ = 1;
    bool on_ = false;
};

// ---- 3. the flow of one link ----
// What the builder applies: n players (0 = nothing: the native roster stands), each one's battle slot and agreed pick, this PC's slot.
struct PairOut {
    int n = 0;
    uint8_t self_slot = 0;
    uint8_t slot[jspk::kMaxPlayers] = {};
    jspk::Pick pick[jspk::kMaxPlayers];
    jspk::TeamMode mode() const { return n ? jspk::agreed_mode(pick) : jspk::VERSUS; }
};
// What the main thread must do now (poll): each is returned once per post-game.
enum class Act : uint8_t {
    None = 0,
    LeaveGiveUp,   // this player left (its choice or the idle timeout): close the link, no rollback again in this native room
    LeaveClose,    // another player left or the link went silent: close the link; native pairing may open a new one for whoever stays
    Rematch,       // Fight again: back to the room (auto-skip), the round only carries "in the room"
    CharSelect,    // Select character: back to the room, then this player picks
    Ready,         // every player's pick is locked and every PC holds them all (the host may start)
    Lost,          // the pick round lost a player: like LeaveClose
    PeerOff,       // FIX (pick7): another PC did not open its prompt for this match (no switch agreement there): this prompt closed, the link stays
    Froze,         // FIX (pick7): this PC's game made no flip for kFreezeMs with the menu up: the pump thread left for it (see 3); close the link
    PeerFroze      // FIX (pick7): another PC's game froze with the menu up (its hello says gone): close the link, like LeaveClose
};
inline const char* act_name(Act a)
{
    switch (a) {
    case Act::None: return "none"; case Act::LeaveGiveUp: return "leave (this player)"; case Act::LeaveClose: return "leave (another player / link)";
    case Act::Rematch: return "fight again"; case Act::CharSelect: return "select character"; case Act::Ready: return "picks agreed"; case Act::Lost: return "pick round lost";
    case Act::PeerOff: return "the other PC's menu is off for this match";
    case Act::Froze: return "this game froze in the menu"; case Act::PeerFroze: return "the other PC's game froze in the menu";
    }
    return "?";
}

class Flow {
public:
    // A rollback link's plan is known: me = this PC's player index (its peer id; 0 = the host), n = players 2..4, slots[i] = player i's battle slot
    // (its room record index). false: refused (bad sizes, slots not distinct). The pick messages carry session_tag(plan_session): an older link's are refused.
    bool open(uint64_t plan_session, int me, int n, const uint8_t* slots)
    {
        *this = Flow();
        if (n < 2 || n > jspk::kMaxPlayers || me < 0 || me >= n || !slots) return false;
        for (int i = 0; i < n; i++) { if (slots[i] > 3) return false; for (int j = 0; j < i; j++) if (slots[i] == slots[j]) return false; }
        tag_ = jspk::session_tag(plan_session); me_ = me; n_ = n;
        for (int i = 0; i < n; i++) slots_[i] = slots[i];
        pg_.configure(me, n, tag_);
        room_teams_ = jspk::room_teams_from(std::getenv(jspk::kRoomTeamsKey), std::getenv(jspk::kVersusKey));   // FIX (test13)
        return true;
    }
    void close() { *this = Flow(); }
    bool is_open() const { return n_ >= 2; }
    int players() const { return n_; }
    int me() const { return me_; }
    bool host() const { return is_open() && me_ == 0; }
    void set_config(const jspg::Config& pc, const jspk::Config& kc) { pcfg_ = pc; kcfg_ = kc; }
    // FIX (test13): this PC's room-teams switch (open() reads JSP_PICK_ROOM_TEAMS; only the host's counts, jspk::agreed_mode). Before a round.
    void set_room_teams(bool on) { room_teams_ = on; }
    bool room_teams() const { return room_teams_; }

    // One datagram of the link's settings channel from peer id `peer` (the link's sender, = that player's index). true: a 'P' / 'C' message,
    // consumed here (also when this link runs no prompt, so the handshake objects never see one). A message whose player index is not its
    // sender's (or from no other player of this link) is dropped and counted (spoofed). Any datagram proves the link alive for the 2-player choice.
    bool route(int peer, const uint8_t* d, size_t len, uint64_t ms)
    {
        if (!d || !len) return false;
        if (d[0] != kTagPostgame && d[0] != kTagPick) { if (is_open()) pg_.link_alive(ms); return false; }
        routed_++;
        Hello h;
        const bool hello = d[0] == kTagPostgame && hello_decode(d + 1, len - 1, &h);
        if (!is_open()) { if (hello) off_hellos_++; return true; }   // FIX (pick7): this PC runs no prompt; the other PC's hello is counted
        if (peer < 0 || peer >= n_ || peer == me_) { spoofed_++; return true; }
        if (hello) {
            if (h.player != peer) { spoofed_++; return true; }
            if (h.session != tag_) return true;   // an older link's
            pg_.link_alive(ms);
            const uint8_t bit = (uint8_t)(1u << peer);
            if (!(hello_heard_ & bit)) { hello_heard_ |= bit; hello_dirty_ = true; }   // answer at once: the other PC learns it was heard
            if (h.heard & (1u << me_)) peer_heard_me_ |= bit;
            if (h.gone && pg_.on() && !peer_gone_) peer_gone_ = true;   // FIX (pick7): that PC's game froze with the menu up
            if (h.declined) {
                peer_declined_[peer] = h.declined;
                if (pg_.on() && pg_.outcome() == jspg::PENDING && h.declined == (uint16_t)(pg_.seq() + 1u)) { pg_.stop(); peer_off_ = true; reset_pair_teams(); }
            }
            return true;
        }
        pg_.link_alive(ms);
        if (d[0] == kTagPick) {
            jspk::Msg m;
            if (!jspk::decode(d + 1, len - 1, &m)) return true;
            if (m.player != peer) { spoofed_++; return true; }
            pick_.receive(m, ms);
            on_round_change();
            return true;
        }
        if (n_ > 2) {   // the 3..4 player message names its sender; the 2-player one has no player field (its only possible sender is checked above)
            GroupMsg g;
            if (group_decode(d + 1, len - 1, &g) && g.player != peer) { spoofed_++; return true; }
        }
        pg_.receive_bytes(d + 1, len - 1, ms);
        return true;
    }
    // Every pump tick: the timeouts, and the messages to send (send(bytes, n) to every other player, the tag byte included).
    template <class Send> void pump(uint64_t t, Send&& send)
    {
        if (!is_open()) return;
        uint8_t b[1 + (Postgame::kMaxMsg > jspk::kMsgLen ? Postgame::kMaxMsg : jspk::kMsgLen)];
        static_assert(kHelloLen <= Postgame::kMaxMsg, "the hello fits the send buffer");
        // FIX (pick7): the freeze owner while the menu is up (see 3 at the top): no flip for kFreezeMs since the later of the last one and the
        // match end. The prompt's own choice becomes Leave when none was made; the pick round sends nothing more; every hello says gone.
        if (!frozen_ && pg_.on() && alive_ms_ && t >= alive_ms_ + kFreezeMs) {
            frozen_ = true; froze_new_ = true; hello_dirty_ = true;
            if (pg_.outcome() == jspg::PENDING && pg_.mine() == jspg::NONE) pg_.pick(jspg::LEAVE, t);
            if (pick_on_) pick_.stop();
        }
        if (hello_dirty_ || t >= hello_next_) {   // FIX (pick7): the switch handshake
            Hello h; h.session = tag_; h.heard = hello_heard_; h.player = (uint8_t)me_; h.declined = declined_; h.gone = frozen_;
            b[0] = kTagPostgame; hello_encode(h, b + 1); send(b, 1 + kHelloLen); sent_++;
            hello_dirty_ = false; hello_next_ = t + (agreed() && !frozen_ ? kHelloSlowMs : kHelloMs);
        }
        pg_.tick(t);
        if (const size_t n = pg_.take(t, b + 1)) { b[0] = kTagPostgame; send(b, n + 1); sent_++; }
        if (pick_on_) {
            pick_.tick(t);
            on_round_change();
            jspk::Msg m;
            if (pick_.take(t, &m)) { b[0] = kTagPick; jspk::encode(m, b + 1); send(b, 1 + jspk::kMsgLen); sent_++; }
        }
    }
    // A match of this link ended normally (seq = its match number, the same on every PC): the prompt opens.
    void match_over(uint16_t seq, uint64_t now)
    {
        if (!is_open()) return;
        pick_.stop(); pick_on_ = false; acted_ = false; ready_said_ = false; rematch_ = false; round_pair_ = false; pg_left_ = false;
        committed_ = false; want_room_ = false; peer_off_ = false; peer_off_said_ = false; decline_why_ = 0;
        frozen_ = froze_new_ = froze_said_ = peer_gone_ = peer_gone_said_ = false; if (now > alive_ms_) alive_ms_ = now;
        pg_.start(seq, now, pcfg_);
    }
    // FIX (pick7): the match end as the game calls it: the prompt opens only when every PC agreed on the switch (see 1b) and no other PC
    // declined this match already. Else this match is declined (said in the next hello, at once) and false is returned: no prompt, no hold,
    // the game goes on as with the switch off. decline_why(): 1 = no agreement, 2 = another PC declined this match first.
    bool match_end(uint16_t seq, uint64_t now)
    {
        if (!is_open()) return false;
        int why = agreed() ? 0 : 1;
        for (int i = 0; i < n_ && !why; i++) if (i != me_ && peer_declined_[i] == (uint16_t)(seq + 1u)) why = 2;
        match_over(seq, now);
        if (!why) return true;
        pg_.stop(); decline_why_ = why; declined_ = (uint16_t)(seq + 1u); hello_dirty_ = true;
        reset_pair_teams();   // FIX (test13 integration review 2): no round for this match on any PC
        return false;
    }
    // FIX (test13 integration review 1): this PC's match `seq` ended abnormally (a refused rollback restore, the NULL-call guard, a desync, a
    // failure: not jkr::normal_end) after it ran, so this PC opens no menu for it. Before, it said nothing: another PC that had ended that match
    // normally sat in its prompt (2 players: the idle timeout, 180 s, then gave the room up; 3-4: "link lost" after 8 s and a re-plan that
    // needed this PC). Now the next hello (at once) declines the match: a prompt open elsewhere closes (Act::PeerOff, no hold, the link stays),
    // and a PC that ends it normally later declines too (match_end, why 2). jsp_net_game.cpp end_with calls it under the pump mutex.
    void decline(uint16_t seq)
    {
        if (!is_open()) return;
        declined_ = (uint16_t)(seq + 1u); hello_dirty_ = true;
        reset_pair_teams();
    }
    // FIX (pick7): every other player's hello is held and says it heard this PC's.
    bool agreed() const
    {
        if (!is_open()) return false;
        for (int i = 0; i < n_; i++) if (i != me_ && (!(hello_heard_ & (1u << i)) || !(peer_heard_me_ & (1u << i)))) return false;
        return true;
    }
    int decline_why() const { return decline_why_; }
    uint64_t off_hellos() const { return off_hellos_; }
    // FIX (pick7): the freeze owner. game_alive: the game made a new flip (the pump thread polls the flip count; any thread under the caller's
    // lock). take_froze: the freeze was found since the last call (the pump thread logs it). frozen: it was found for this match.
    void game_alive(uint64_t now) { if (now > alive_ms_) alive_ms_ = now; }
    bool take_froze() { const bool f = froze_new_; froze_new_ = false; return f; }
    bool frozen() const { return frozen_; }
    // Main thread: the next step to act on (each one once).
    Act poll(uint64_t now)
    {
        if (peer_off_ && !peer_off_said_) { peer_off_said_ = true; return Act::PeerOff; }   // FIX (pick7): another PC declined this match
        if (frozen_ && !froze_said_) { froze_said_ = true; end_round(); return Act::Froze; }             // FIX (pick7): the freeze owner left for this PC
        if (peer_gone_ && !peer_gone_said_) {
            peer_gone_said_ = true;
            // FIX (test13 review, REVIEW-MENU4 5 / REVIEW-QUIT 3): the gone hello came with that player's Leave (a window closed, or a freeze with no
            // choice made): the normal outcome below says "Opponent left" / "A player left" (Act::LeaveClose); the link closes the same way
            const bool left = pg_.on() && !acted_ && pg_.outcome() == jspg::O_LEAVE && pg_.reason() == jspg::R_PEER_LEFT;
            if (!left) { end_round(); return Act::PeerFroze; }
        }
        if (!is_open() || !pg_.on()) return Act::None;
        const jspg::Outcome o = pg_.outcome();
        if (o == jspg::PENDING) return Act::None;
        if (!acted_) {
            acted_ = true;
            if (o == jspg::O_LEAVE) { pg_left_ = true; return pg_.reason() == jspg::R_SELF || pg_.reason() == jspg::R_IDLE ? Act::LeaveGiveUp : Act::LeaveClose; }
            rematch_ = o == jspg::O_REMATCH;
            round_pair_ = !rematch_ || pair_.n > 0;   // Fight again without an earlier pick: the native roster stands
            jspk::Pick prev = pair_.n ? pair_.pick[me_] : jspk::Pick();
            // FIX (test13 review L3): every round starts from the room's teams (Versus without room teams): an earlier round's explicit Versus /
            // Together never carries on, and a Fight again (prev locked at once) always plays the room's teams
            jspk::set_choice(prev, room_teams_ ? jspk::CHOICE_ROOM : jspk::CHOICE_VERSUS);
            pick_.start(tag_, pg_.seq(), me_, n_, prev, now, kcfg_);
            pick_on_ = true;
            if (rematch_) pick_.lock();
            return rematch_ ? Act::Rematch : Act::CharSelect;
        }
        if (pick_on_ && !ready_said_) {
            if (pick_.state() == jspk::READY) { ready_said_ = true; on_round_change(); return Act::Ready; }
            if (pick_.state() == jspk::LOST) { ready_said_ = true; return Act::Lost; }
        }
        return Act::None;
    }
    // The native re-arm of the next match waits: the choice is open, or the pick round has not finished.
    bool hold() const
    {
        if (!is_open() || !pg_.on() || pg_left_) return false;
        if (pg_.outcome() == jspg::PENDING || !acted_) return true;
        return pick_on_ && pick_.state() != jspk::READY && pick_.state() != jspk::LOST;
    }
    // The next fight runs (pump thread): the round stops (a READY round was committed when it became READY).
    void fight_running()
    {
        if (!pick_on_) { pg_.stop(); return; }
        on_round_change();
        pick_.stop(); pick_on_ = false; pg_.stop(); want_room_ = false;
    }

    // UI
    bool choose(jspg::Choice c, uint64_t now) { return is_open() && pg_.pick(c, now); }
    // FIX (test11 field, room 77; re-done in test13 with the review fixes of D:\jsp-audit\test12\REVIEW-QUIT.md): this PC's window is closing.
    // As the freeze owner does (pump(), 3 at the top): the prompt's own choice becomes Leave when none was made, the pick round sends nothing
    // more, and every hello says gone, so every other PC closes its round at once (Act::PeerFroze) instead of after silence_ms of nothing
    // ("left or the connection was lost"). The caller pumps at once under the same lock (the pump thread's tick is 100 ms, and the game thread
    // polls in between); the pump thread then keeps resending while the process lives (ps3_host_quit's grace). This PC's poll() is inert from
    // here on (acted_, pg_left_ and the said-flags set), so nothing on the game thread closes the link before those datagrams are out.
    // true = told (once; false before the prompt: a fight is the session's own disconnect).
    bool quitting(uint64_t now)
    {
        if (!is_open() || !pg_.on() || frozen_) return false;
        frozen_ = true; froze_said_ = true; peer_gone_said_ = true; hello_dirty_ = true;   // froze_said_: no Act::Froze for a process that is exiting
        if (pg_.outcome() == jspg::PENDING && pg_.mine() == jspg::NONE) pg_.pick(jspg::LEAVE, now);
        if (pick_on_) pick_.stop();
        acted_ = true; pg_left_ = true;   // REVIEW-QUIT 1b: no Act::LeaveGiveUp -> native_close on the game thread before the Leave is sent
        return true;
    }
    // FIX (test13): the caller's team choice stands (jspk::set_choice: the room's teams by default, an explicit Versus / Together this round);
    // without room teams (JSP_PICK_ROOM_TEAMS=0) the room bit is always off.
    bool submit(jspk::Pick p, bool lock) { if (!room_teams_) p.room = false; if (!pick_on_ || !pick_.set_pick(p)) return false; return !lock || pick_.lock(); }
    // This PC reached its room page. The flag goes out only once this PC's round is READY (see the top: the host's start then proves that
    // every PC committed the same picks).
    void in_room(bool v) { want_room_ = v; on_round_change(); }
    bool picking() const { return pick_on_ && pick_.state() == jspk::PICKING; }
    bool rematch_round() const { return rematch_; }
    // The host may press "Go to battle": every pick agreed and every player in its room.
    bool start_ok() const { return pick_on_ && pick_.state() == jspk::READY && pick_.all_in_room(); }
    // The host's own Cross on the room page must not reach the game: a round runs and the start is not yet safe (jsp_net_game.cpp swallows it).
    bool block_start() const { return host() && pick_on_ && pick_.state() != jspk::LOST && !start_ok(); }
    const Postgame& postgame() const { return pg_; }
    const jspk::PickAgreement& round() const { return pick_; }
    bool pick_on() const { return pick_on_; }

    // What the builder applies now: the last committed pair (a round is committed when it becomes READY on this PC; a round of Fight again
    // without an earlier pick commits nothing). false: nothing (the native roster stands).
    bool pair(PairOut* out) const
    {
        if (!pair_.n) return false;
        *out = pair_;
        return true;
    }
    // versus_apply stays out: a pair was committed on this link, or this post-game's round will commit one (decided by the agreed outcome, the
    // same on every PC, not by when each PC locks). apply_pair sets the teams itself, whatever versus_apply did before.
    bool active() const { return pair_.n > 0 || (pick_on_ && round_pair_); }
    uint64_t spoofed() const { return spoofed_; }
    uint64_t routed() const { return routed_; }
    uint64_t sent() const { return sent_; }

private:
    PairOut round_out() const
    {
        PairOut p; p.n = n_; p.self_slot = slots_[me_];
        for (int i = 0; i < n_; i++) { p.slot[i] = slots_[i]; p.pick[i] = pick_.pick_of(i); }
        return p;
    }
    // FIX (test13 integration review 2): a match that ends without a round on every PC (this PC or another declined it: an abnormal end, a
    // missing switch) keeps the committed pair for the next fight's builder, but its team choice goes back to the room default (the room's
    // teams, Versus without them), so an earlier round's explicit Together / Versus never carries past its own fight ("team choice per round").
    // Every PC does this for the same match (decline, match_end's decline, the PeerOff of an open prompt), so every builder applies the same pair.
    void reset_pair_teams()
    {
        for (int i = 0; i < pair_.n; i++) jspk::set_choice(pair_.pick[i], room_teams_ ? jspk::CHOICE_ROOM : jspk::CHOICE_VERSUS);
    }
    // FIX (pick7): the menu of this match is over for this PC without an agreed outcome (a freeze here or there): no hold, no round, no messages.
    void end_round() { acted_ = true; pg_left_ = true; pick_.stop(); pick_on_ = false; pg_.stop(); want_room_ = false; }
    // READY: the round is committed (once); then this PC's "in the room" flag may go out.
    void on_round_change()
    {
        if (!pick_on_ || pick_.state() != jspk::READY) return;
        if (round_pair_ && !committed_) { pair_ = round_out(); committed_ = true; }
        if (want_room_ && !pick_.in_room_of(me_)) pick_.set_in_room(true);
    }

    uint32_t tag_ = 0;
    int me_ = 0, n_ = 0;
    uint8_t slots_[jspk::kMaxPlayers] = {};
    jspg::Config pcfg_;
    jspk::Config kcfg_;
    Postgame pg_;
    jspk::PickAgreement pick_;
    bool room_teams_ = true;   // FIX (test13)
    bool pick_on_ = false, acted_ = false, ready_said_ = false, rematch_ = false, round_pair_ = false, pg_left_ = false, committed_ = false, want_room_ = false;
    PairOut pair_;
    uint64_t routed_ = 0, sent_ = 0, spoofed_ = 0;
    // FIX (pick7): the switch handshake (1b)
    uint8_t hello_heard_ = 0, peer_heard_me_ = 0;
    uint16_t declined_ = 0, peer_declined_[jspk::kMaxPlayers] = {};
    uint64_t hello_next_ = 0, off_hellos_ = 0;
    bool hello_dirty_ = true, peer_off_ = false, peer_off_said_ = false;
    int decline_why_ = 0;
    // FIX (pick7): the freeze owner (3)
    uint64_t alive_ms_ = 0;
    bool frozen_ = false, froze_new_ = false, froze_said_ = false, peer_gone_ = false, peer_gone_said_ = false;
};

// ---- 4. the roster write of an agreed pair on a room image ----
enum class MemberMap : uint8_t { Slot = 0, Order = 1, None = 2 };
inline const char* member_map_text(MemberMap m) { return m == MemberMap::Slot ? "by battle slot" : m == MemberMap::Order ? "by record order (fallback, U5)" : "none"; }
// hp[i] = player i's member id and pick. Slot: the record at its battle slot is a human with a member id. Order (fallback): the n human records in
// record order go to the players in battle-slot order. None: not n human records.
inline MemberMap members_of(const uint8_t* img, const PairOut& p, jspk::HumanPick* hp)
{
    auto rec = [img](int r) { return img + jspk::kOffRecords + jspk::kRecSize * (uint32_t)r; };
    bool ok = true;
    for (int i = 0; i < p.n; i++) {
        const uint8_t* r = rec(p.slot[i] & 3);
        hp[i].pick = p.pick[i]; hp[i].member = r[jspk::kRecCom] ? 0 : jspk::be16(r + jspk::kRecMember);
        if (!hp[i].member) ok = false;
    }
    if (ok) return MemberMap::Slot;
    int hum[4], nh = 0;
    for (int r = 0; r < 4; r++) if (!rec(r)[jspk::kRecCom] && jspk::be16(rec(r) + jspk::kRecMember)) hum[nh++] = r;
    if (nh != p.n) return MemberMap::None;
    int order[4] = { 0, 1, 2, 3 };
    for (int i = 1; i < p.n; i++) for (int j = i; j > 0 && p.slot[order[j]] < p.slot[order[j - 1]]; j--) { const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (int k = 0; k < p.n; k++) hp[order[k]].member = jspk::be16(rec(hum[k]) + jspk::kRecMember);
    return MemberMap::Order;
}
// The roster write on a whole room image (the builder hook's copy of R). self = this PC's member id gets its own pick block (R+0x08 / +0x0C).
inline jspk::Result apply_pair(uint8_t img[jspk::kRoomSize], const PairOut& p, jspk::ApplyInfo* info = nullptr, MemberMap* how = nullptr)
{
    jspk::HumanPick hp[jspk::kMaxPlayers];
    const MemberMap m = members_of(img, p, hp);
    if (how) *how = m;
    if (m == MemberMap::None) return jspk::Result::HumansMismatch;
    // ROOM (test13, the default): the room's own record teams (see the top). VERSUS / TOGETHER: the teams only from the member ids, the record
    // order and the mode: the record team bytes as "unset" (see the top); the slot team bytes keep whether they are set (0/1) or not (2),
    // which versus_apply never changes
    const jspk::TeamMode mode = p.mode();
    jspk::Roster in = jspk::roster_read(img), out;
    if (mode != jspk::ROOM) for (int r = 0; r < 4; r++) in.rec[r].set_team(2);
    const jspk::Result res = jspk::roster_apply(in, hp, p.n, mode, &out, info);
    if (res == jspk::Result::Ok) jspk::roster_write(out, img);   // the own pick block R+0x08 / +0x0C stays as it is (it differs between PCs)
    return res;
}

// ---- 5. the picker's texts ----
// The fighter list: Random first, then every id of the select screen's stock list (in its order) that has a name, once each. With no
// named id in the list (not read, a mod's list): every named fighter alphabetically. Returns the count written (<= cap).
inline size_t picker_fighters(const uint32_t* stock, size_t n, uint16_t* out, size_t cap)
{
    size_t k = 0;
    auto add = [&](uint16_t id) { for (size_t i = 0; i < k; i++) if (out[i] == id) return; if (k < cap) out[k++] = id; };
    add(jspk::kRandom);
    for (size_t i = 0; stock && i < n; i++) if (stock[i] < 0x10000u && jnames::fighter_name((int)stock[i])) add((uint16_t)stock[i]);
    if (k > 1) return k;
    uint16_t ids[42]; size_t m = 0;
    for (int id = 0; id < 42; id++) if (jnames::fighter_name(id)) ids[m++] = (uint16_t)id;
    for (size_t i = 1; i < m; i++)
        for (size_t j = i; j > 0 && std::string(jnames::fighter_name(ids[j])) < jnames::fighter_name(ids[j - 1]); j--) { const uint16_t t = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = t; }
    for (size_t i = 0; i < m; i++) add(ids[i]);
    return k;
}
// The Support list: Random (the room's own support) first, then the named Supports alphabetically, without the stub the game never
// offers (jnames::kSupportStub). The game's own Support order is not known (TODO-MAPPING U1b also: the units the room holds).
inline size_t picker_supports(uint16_t* out, size_t cap)
{
    size_t k = 0;
    if (k < cap) out[k++] = jspk::kRandom;
    uint16_t ids[jnames::kSupportEnd - jnames::kSupportFirst]; size_t m = 0;
    for (int id = jnames::kSupportFirst; id < jnames::kSupportEnd; id++) if (id != jnames::kSupportStub && jnames::support_name(id)) ids[m++] = (uint16_t)id;
    for (size_t i = 1; i < m; i++)
        for (size_t j = i; j > 0 && std::string(jnames::support_name(ids[j])) < jnames::support_name(ids[j - 1]); j--) { const uint16_t t = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = t; }
    for (size_t i = 0; i < m && k < cap; i++) out[k++] = ids[i];
    return k;
}
inline std::string fighter_text(uint16_t id)
{
    if (id == jspk::kRandom) return "Random";
    const char* n = jnames::fighter_name(id);
    return n ? std::string(n) : "No. " + std::to_string((unsigned)id);
}
inline std::string support_text(uint16_t id)
{
    if (id == jspk::kRandom) return "Room setting";   // the support the room already has
    const char* n = jnames::support_name(id);
    return n ? std::string(n) : "No. " + std::to_string((unsigned)id);
}
// s with every byte outside 0x20..0x7E as '?' (the menu font's range), cut to max_w pixels by width(text) with "..." at the end. A width
// of 0 (the font atlas is missing) keeps the text whole.
template <class Width> std::string fit_text(const std::string& in, float max_w, Width&& width)
{
    std::string s;
    for (char c : in) s += (c >= 0x20 && c <= 0x7E) ? c : '?';
    const float w = width(s);
    if (w <= 0 || w <= max_w) return s;
    while (!s.empty()) {
        s.pop_back();
        while (!s.empty() && s.back() == ' ') s.pop_back();
        const std::string t = s + "...";
        if (width(t) <= max_w) return t;
    }
    return "...";
}
// One player's pick for everyone to see: "P2: Goku + Rukia, locked" ("P1 (you): Random + Room setting, choosing").
inline std::string pick_line(int player, int me, const jspk::Pick& p, bool locked)
{
    return "P" + std::to_string(player + 1) + (player == me ? " (you)" : "") + ": " + fighter_text(p.fighter) + " + " + support_text(p.support) +
           (locked ? ", locked" : ", choosing");
}

}  // namespace jspf
