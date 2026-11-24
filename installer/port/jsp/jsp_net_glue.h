// jsp_net_glue.h -- ENHANCEMENT (rollback netcode): the main-loop glue between NetSession (GekkoNet events) and the stepper.
// No game addresses: the game supplies IGlueGame, the host test (installer/tools/net_session_test.cpp) supplies a fake game; both run THIS code.
//
//
// ONE iteration of the main loop calls Glue::iteration() exactly once and gets either Run (the simulation runs this tick) or None (it must not).
// Inside it, in this order (the order is the fix for the half-rate bug: Save events of the last tick are only committed at the NEXT boundary, so a loop that
// updates GekkoNet first finds `saves pending`, defers, and the live tick is granted one iteration later):
//   1. boundary()   commits the Save(s) of the tick that just ran (complete_save with the snapshot hash), executes Rollbacks, returns Run for queued
//                   events (a replay tick) or None when the queue is empty;
//   2. on None, once per iteration: gekko update() with this iteration's local pads, convert the events and push them to the stepper;
//   3. boundary() again: Run for the live tick (or a Commit/Rollback first, then Run).
// A Run ends the iteration at once (one tick per iteration, live or replayed). The Run op is returned AGAIN, with regrant = true, while the granted tick has not
// completed; the game then must not poll the pad again (see Stepper::boundary: skip_pad).
#pragma once
#include <cstdio>
#include <map>
#include "jsp_net_session.h"
#include "jsp_stepper.h"

namespace jspnet {

class IGlueGame {
public:
    virtual ~IGlueGame() {}
    virtual uint32_t done() = 0;                                              // completed simulation ticks
    // The state with `tick` completed is a rollback target now: take the snapshot. Returns false on failure (the session ends).
    virtual bool commit(uint32_t tick, uint32_t* hash, uint32_t* ring_token) = 0;
    virtual bool rollback(uint32_t tick) = 0;                                 // restore the state with `tick` completed
    // ENHANCEMENT (rollback latency): the checksum of `tick` WITHOUT a snapshot, for a save whose state can never be loaded (see Glue::set_skip_final).
    // false = not available: the glue takes the full commit instead.
    virtual bool hash_only(uint32_t tick, uint32_t* hash) { (void)tick; (void)hash; return false; }
    // The local pad of local player n for the update about to run, canonical wire form (pad_pack_to_wire). Called once per update per local player.
    virtual void local_pad(uint8_t local_player, uint8_t wire[8]) = 0;
    // ENHANCEMENT (match perf summary, jsp_net_perf.h): every Advance event as it arrives (GekkoNet frame, replay or live, the inputs of all slots). Observe only.
    virtual void advanced(int32_t frame, bool rolling_back, const jsi::Pad* in, unsigned players) { (void)frame; (void)rolling_back; (void)in; (void)players; }
};

class Glue {
public:
    Glue(NetSession& s, jsx::Stepper& st, IGlueGame& g) : s_(s), st_(st), g_(g) {}
    // allow_update=false: pacing (frames_ahead >= 1) or the caller is not ready; the stepper is still serviced (commits, replays).
    jsx::Op iteration(bool allow_update = true)
    {
        bool updated = false, polled = false;
        for (;;) {
            if (s_.failed()) return none();
            const jsx::Op op = st_.boundary(g_.done());
            switch (op.kind) {
            case jsx::OpKind::Commit: {
                uint32_t h = 0, tok = 0;
                if (save_ids_.empty()) { s_.abort("commit failed"); return none(); }
                // ENHANCEMENT (rollback latency): a save of a frame below GekkoNet's final frame can never be loaded (every later Load names the final frame or a
                // later one), so it gets its checksum (desync detection) but no snapshot: no freeze, no copy. The tick is then no rollback target (Stepper::forget).
                // ENHANCEMENT (test10, snap-poll): nothing polls the socket during the ~30 ms tick (the keepalive thread only after 250 ms of silence), so
                // without a poll here the final frame is the one of the last update: a remote input that arrived during the tick does not count yet and the
                // save of a frame that is in fact confirmed takes a full snapshot. One non-blocking poll per iteration, before its first skip test. It cannot
                // change what any frame simulates: GekkoNet's Poll (game_session.cpp Poll -> HandleReceivedInputs) only files received remote inputs into
                // its input buffer (AddRemoteInput, which marks a misprediction as an incorrect frame) and re-sends unacked local inputs; Advance, Load and
                // Save events are made only by UpdateSession, which begins with the very same Poll, so the next update sees exactly the inputs it would have
                // seen anyway (the keepalive thread already polls at arbitrary moments, and so does the snapshot gate's idle_service, the hook jsp_net_game.cpp
                // sets, at most every 20 ms in its retry waits and precheck spins with the threads running). FinalFrame stops before a pending incorrect frame, so a poll that finds
                // a misprediction only lowers it: a frame a later Load can target is never skipped.
                int ff = -1;
                if (skip_final_) {
                    const int ff_before = s_.final_frame();
                    if (!polled) { s_.poll(); polled = true; }
                    ff = s_.final_frame();
                    if (save_frames_.front() < ff && !(save_frames_.front() < ff_before)) poll_skips_++;
                }
                const int sf = save_frames_.front();
                const bool skip = skip_final_ && sf < ff && g_.hash_only(op.tick, &h);
                if (skip) { st_.forget(op.tick); skipped_++; }
                else if (!g_.commit(op.tick, &h, &tok)) { s_.abort("commit failed"); return none(); }
                if (skip_final_) note_arrival(sf, ff, !skip);
                if (!s_.complete_save(save_ids_.front(), h, tok)) { s_.abort("commit failed"); return none(); }
                save_ids_.erase(save_ids_.begin()); save_frames_.erase(save_frames_.begin()); commits_++;
                break;
            }
            case jsx::OpKind::Rollback:
                if (!g_.rollback(op.tick)) { s_.abort("rollback failed"); return none(); }
                break;
            case jsx::OpKind::Error: s_.abort(std::string("stepper: ") + jsx::err_name(op.err)); return none();
            case jsx::OpKind::Run: runs_++; return op;
            case jsx::OpKind::None:
                if (updated || !allow_update) return op;
                updated = true;
                if (!update_once()) return op;
                break;
            }
        }
    }
    uint64_t updates() const { return updates_; }
    uint64_t runs() const { return runs_; }
    uint64_t commits() const { return commits_; }
    uint64_t skipped() const { return skipped_; }   // commits that took no snapshot (final frame)
    uint64_t poll_skips() const { return poll_skips_; }   // of them: skipped only because of the poll before the test (the final frame of the last update was too low)
    void set_skip_final(bool on) { skip_final_ = on; }
    // DIAGNOSTIC (test10, snap-poll): of the saves that took a snapshot (the frame was still predicted), how many ticks passed until GekkoNet's final frame
    // passed that frame (its remote input confirmed it). Ticks = GekkoNet frames, counted on the newest saved frame, so a rollback's re-run saves do not
    // count as time. Checked at each commit (after its poll), so 1 = confirmed by the next tick's save. One line, for the match end.
    std::string arrival_summary() const
    {
        uint64_t n = 0; for (uint64_t c : arr_hist_) n += c;
        char b[256];
        if (!n) { std::snprintf(b, sizeof b, "[net] predicted saves confirmed after: none measured (%llu still open), %llu saves skipped thanks to the poll\n",
                                (unsigned long long)arr_open_.size(), (unsigned long long)poll_skips_); return b; }
        auto pct = [&](double q) { const uint64_t want = (uint64_t)(q * (double)n + 0.999999); uint64_t acc = 0;
                                   for (int i = 0; i < kArrBins; i++) { acc += arr_hist_[i]; if (acc >= want) return i; } return kArrBins - 1; };
        const int p50 = pct(0.5), p90 = pct(0.9);
        std::snprintf(b, sizeof b, "[net] predicted saves confirmed after: p50 %d%s p90 %d%s ticks, max %d ticks (%llu measured, %llu still open, %llu dropped); "
                      "%llu saves skipped thanks to the poll\n", p50, p50 == kArrBins - 1 ? "+" : "", p90, p90 == kArrBins - 1 ? "+" : "", arr_max_,
                      (unsigned long long)n, (unsigned long long)arr_open_.size(), (unsigned long long)arr_dropped_, (unsigned long long)poll_skips_);
        return b;
    }
private:
    static jsx::Op none() { return jsx::Op(); }
    // Arrival bookkeeping (diagnostic only, never feeds a decision): `frame` was just saved with a snapshot (snapshotted) or as a checksum; `ff` = final frame.
    void note_arrival(int frame, int ff, bool snapshotted)
    {
        if (frame > arr_newest_) arr_newest_ = frame;
        if (snapshotted && !arr_open_.count(frame)) {   // a re-save after a rollback keeps the first time the frame was predicted
            if (arr_open_.size() >= kArrOpenMax) { arr_open_.erase(arr_open_.begin()); arr_dropped_++; }
            arr_open_.emplace(frame, arr_newest_);
        }
        while (!arr_open_.empty() && arr_open_.begin()->first < ff) {
            const int d = arr_newest_ - arr_open_.begin()->second;
            const int bin = d < 0 ? 0 : d >= kArrBins ? kArrBins - 1 : d;
            arr_hist_[bin]++; if (d > arr_max_) arr_max_ = d;
            arr_open_.erase(arr_open_.begin());
        }
    }
    // Pad of local player n -> events -> stepper queue. True when events were pushed (the caller calls boundary() again).
    bool update_once()
    {
        const unsigned players = s_.plan().total_players;
        for (unsigned n = 0; n < s_.plan().local_count; n++) {
            uint8_t wire[8];
            g_.local_pad((uint8_t)n, wire);
            s_.set_local_input((uint8_t)n, wire);
        }
        evs_.clear();
        if (!s_.update(evs_, st_.queued() == 0)) return false;
        updates_++;
        for (const NetEvent& e : evs_) {
            jsx::Event je; je.frame = e.frame; je.rolling_back = e.rolling_back;
            switch (e.kind) {
            case EvKind::Save: je.kind = jsx::Kind::Save; save_ids_.push_back(e.token); save_frames_.push_back(e.frame); break;
            case EvKind::Load: je.kind = jsx::Kind::Load; break;
            case EvKind::Advance: {
                je.kind = jsx::Kind::Advance;
                for (unsigned p = 0; p < players && p < (unsigned)jsi::kSlots; p++) {
                    uint8_t canon[8];
                    pad_wire_to_pack(e.inputs.data() + (size_t)p * 8, canon);
                    if (!jsi::Pad::unpack(canon, 8, &je.in[p])) { s_.abort("non-canonical pad from the wire, slot " + std::to_string(p)); return false; }
                    je.confirmed[p] = false;
                }
                g_.advanced(je.frame, je.rolling_back, je.in, players);
                break;
            }
            }
            st_.push(je);
        }
        return st_.queued() != 0;
    }
    NetSession& s_; jsx::Stepper& st_; IGlueGame& g_;
    std::vector<NetEvent> evs_;
    std::vector<uint32_t> save_ids_;
    std::vector<int> save_frames_;   // GekkoNet frame of each pending save, parallel to save_ids_
    bool skip_final_ = false;
    uint64_t updates_ = 0, runs_ = 0, commits_ = 0, skipped_ = 0, poll_skips_ = 0;
    static constexpr int kArrBins = 64;                 // 0..62 ticks, 63 = 63 or more
    static constexpr size_t kArrOpenMax = 1024;         // open predicted frames kept (a stuck final frame cannot grow this without bound)
    std::map<int, int> arr_open_;                       // predicted frame -> newest saved frame when it was first snapshotted
    uint64_t arr_hist_[kArrBins] = {};
    uint64_t arr_dropped_ = 0;
    int arr_newest_ = -1, arr_max_ = 0;
};

} // namespace jspnet
