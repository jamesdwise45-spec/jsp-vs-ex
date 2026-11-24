#!/usr/bin/env python3
"""Two-process rollback session tests (no game, no GPU): starts net_session_test as host and client over localhost UDP and checks them.
ENHANCEMENT (4-player online): MESH below runs 3 and 4 processes (one player each) in a GekkoNet mesh: equal hashes on every peer, slot claims, a peer killed mid-match.
    python3 installer/tools/net_session_test.py --exe PATH [--only NAME] [--ticks N] [--deadline S]
Scenarios and what each must show are in SCENARIOS below; exit 1 on any failure. Build: installer/tools/net_session_test.sh (which also runs it).
The exe may also be the MinGW build when run where Windows programs run (not done by default).
Every wait is bounded by the scenario's deadline (--deadline, 120 s from the host's start, both processes together): a process still running then is killed and the
scenario FAILS as hung, naming who hung and printing both outputs' tails (installer/tools/net_session_driver_test.py tests this with fake processes)."""
import argparse, pathlib, re, subprocess, sys, tempfile, threading, time

# Each process bounds itself (rendezvous and handshake 15 s, the main loop --max-ms, 90 s by default, all from its start); the deadline is that plus a margin.
DEADLINE_S = 120


class Proc:
    """One net_session_test process; a thread reads its output (stdout and stderr) so no read can block the driver."""
    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        self.lines, self.port_or_eof = [], threading.Event()
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line)
            if line.startswith("PORT "): self.port_or_eof.set()
        self.port_or_eof.set()

    def port(self):
        return next((l.split()[1] for l in self.lines if l.startswith("PORT ") and len(l.split()) > 1), None)

    def out(self):
        return "".join(self.lines)


def run(exe, host_args, client_args, tmp, deadline_s=DEADLINE_S):
    """Returns (host rc, host output, client rc, client output, problems); problems names a process that did not exit by the deadline (then killed)."""
    end = time.monotonic() + deadline_s
    left = lambda: max(0.0, end - time.monotonic())
    procs, problems = {}, []
    try:
        h = procs["host"] = Proc([exe, "--role", "host", "--out", str(tmp / "h.log")] + host_args)
        h.port_or_eof.wait(left())
        port = h.port()
        if port is None:
            bare = next((l.strip() for l in h.lines if l.startswith("PORT ")), None)
            problems.append(f"host printed a PORT line without a port number: {bare!r}" if bare is not None else
                            "host printed no PORT line" + (f" and exited ({h.p.poll()})" if h.p.poll() is not None else f" within the {deadline_s} s deadline"))
        else:
            procs["client"] = Proc([exe, "--role", "client", "--peer", f"127.0.0.1:{port}", "--out", str(tmp / "c.log")] + client_args)
        for who, pr in procs.items():
            try: pr.p.wait(timeout=left())
            except subprocess.TimeoutExpired: problems.append(f"hung: the {who} was still running at the {deadline_s} s scenario deadline (killed)")
    finally:
        for pr in procs.values():
            if pr.p.poll() is None: pr.p.kill()
            pr.p.wait()
            pr.t.join(10)
    h, c = procs.get("host"), procs.get("client")
    return (h.p.returncode if h else None, h.out() if h else "", c.p.returncode if c else None, c.out() if c else "", problems)


def tails(tmp, ho, co, n=8):
    """Both processes' output tails and their tick logs' tails (for a hung or broken scenario)."""
    out = []
    for who, text, log in (("host", ho, "h.log"), ("client", co, "c.log")):
        out.append(f"{who} output (last {n} lines):")
        out += ["  " + l for l in text.splitlines()[-n:]] or ["  (none)"]
        p = tmp / log
        out.append(f"{who} {log} (last {n} lines): " + ("" if p.exists() else "(not written: written at the end of a run)"))
        if p.exists(): out += ["  " + l for l in p.read_text(errors="replace").splitlines()[-n:]]
    return out


def summary(out):
    m = [l for l in out.splitlines() if l.startswith("SUMMARY")]
    return m[-1] if m else ""


def num(s, k, default=None):
    m = re.search(r"\b%s=(-?\d+)" % k, s)
    return int(m.group(1)) if m else default


def logs(tmp):
    def rd(p):
        t = p.read_text().split() if p.exists() else []   # a missing log reads as empty here; check() fails every kind that compares logs on it (LOG_KINDS)
        return {int(t[i]): t[i + 1] for i in range(0, len(t), 2)}
    return rd(tmp / "h.log"), rd(tmp / "c.log")


# The kinds whose checks compare the two tick logs: each needs both logs with ticks in them, or a hash comparison over nothing would pass.
LOG_KINDS = ("agree", "agree_rb", "agree_paced", "busy_ok", "cheat", "cheat_limited", "leave", "replay")
REPLAY_EXE = None   # --replay-exe: net_replay_test (the "replay" kind plays both peers' recordings back with it)


def agree(tmp, upto):
    h, c = logs(tmp)
    common = [t for t in h if t in c and t <= upto]
    bad = [t for t in common if h[t] != c[t]]
    return len(common), bad[:3]


IMP = ["--loss", "50", "--jitter", "20", "--delay", "10"]
ADV = ["--loss", "200", "--jitter", "40", "--delay", "10"]
SCENARIOS = {
    # name: (host args, client args, kind). Ticks are completed simulation ticks (stepper tick numbering: Advance(F) = tick F+1).
    "clean_2x2": (["--local", "2"], ["--local", "2"], "agree"),
    # Runs (live + replay) per main-loop iteration, window = first tick..tick T: the old two-step loop ran about 0.5. Live ticks per iteration (rate=) are lower by design: every
    # rollback replays ticks at one per iteration (the fake game's inputs change every 3 frames: ~0.63 live, 1.00 Runs); the game's replay clock makes those iterations cheap
    "tick_rate": (["--local", "2", "--change", "40", "--min-rate", "0.9", "--min-all-rate", "0.95"], ["--local", "2", "--change", "40", "--min-rate", "0.9", "--min-all-rate", "0.95"], "rate"),
    # dense inputs (a change every 3 loops, hundreds of rollbacks): every replayed tick costs an iteration, so only Runs per iteration is bounded
    "tick_rate_dense": (["--local", "2", "--min-all-rate", "0.95"], ["--local", "2", "--min-all-rate", "0.95"], "rate_all"),
    # unequal frame times (the pacing must not run away: total pacing sleep under 5% of the run, both peers reach every tick)
    "unequal_tick_rate": (["--local", "2", "--tick-ms", "6", "--delay", "10"], ["--local", "2", "--tick-ms", "10", "--delay", "10"], "agree_paced"),
    # a peer that polls but does not update for 1.5 s (a long commit gate with the idle hook) while the other leaves: the Disconnected raised by those polls must still reach the game
    "leave_during_poll_stall": (["--local", "2", "--poll-stall", "195,1500"], ["--local", "2", "--leave", "200"], "leave"),
    # the game thread is busy for 7 s (a replay behind a slow render) and does not call the session; the 5 s disconnect timer of the peer must not fire because the
    # keepalive thread polls (GekkoNet's Poll sends its NetworkHealth ping). Control: the same without the thread, the peer must time out.
    "busy_stall_keepalive": (["--local", "2", "--busy-stall", "195,7000", "--timeout-ms", "5000"], ["--local", "2", "--timeout-ms", "5000"], "busy_ok"),
    # FIX (review of PR 248, the freeze timeout's owner): the same 7 s stall of the game thread, but another thread ends the session 1 s into it (the game's
    # pump thread on the frame clock's failure): the peer must see the disconnect while the stalled thread is still away, where the keepalive alone kept it waiting
    "stall_owner_abort": (["--local", "2", "--busy-stall", "195,7000", "--stall-abort", "1000", "--timeout-ms", "5000"], ["--local", "2", "--timeout-ms", "5000"], "stall_owner"),
    "busy_stall_no_keepalive": (["--local", "2", "--busy-stall", "195,7000", "--timeout-ms", "5000", "--keepalive-ms", "0"], ["--local", "2", "--timeout-ms", "5000"], "busy_dies"),
    "impaired_2x2": (["--local", "2"] + IMP, ["--local", "2"] + IMP, "agree_rb"),
    "adv_20pct_loss": (["--local", "2"] + ADV, ["--local", "2"] + ADV, "agree_rb"),
    "adv_80ms_jitter": (["--local", "2", "--loss", "50", "--jitter", "80", "--delay", "20"], ["--local", "2", "--loss", "50", "--jitter", "80", "--delay", "20"], "agree_rb"),
    "adv_oneside_30pct": (["--local", "2", "--loss", "300"], ["--local", "2"], "agree_rb"),
    "split_1_3": (["--local", "1"] + ADV, ["--local", "3"] + ADV, "agree_rb"),
    "split_3_1_delay": (["--local", "3", "--delays", "1,1,1"] + IMP, ["--local", "1", "--delays", "2"] + IMP, "agree_rb"),
    "cross_peer_delays": (["--local", "2", "--delays", "1,1"] + IMP, ["--local", "2", "--delays", "3,3"] + IMP, "agree_rb"),
    "cross_peer_delays_adv": (["--local", "2", "--delays", "3,3"] + ADV, ["--local", "2", "--delays", "0,0"] + ADV, "agree_rb"),
    "dirty_padding": (["--local", "2", "--dirty", "1"], ["--local", "2", "--dirty", "1"], "agree"),
    "blackout_resume": (["--local", "2", "--blackout", "1000,2500", "--timeout-ms", "8000"], ["--local", "2", "--blackout", "1000,2500", "--timeout-ms", "8000"], "agree_rb"),
    "cheat": (["--local", "2"], ["--local", "2", "--cheat", "300"], "cheat"),
    "leave": (["--local", "2"], ["--local", "2", "--leave", "200"], "leave"),
    "settings_mismatch": (["--local", "2", "--settings", "a"], ["--local", "2", "--settings", "b"], "mismatch"),
    # refusals: GekkoNet desyncs with unequal local delays on one peer; the session must refuse to start
    "refuse_unequal_3_0": (["--local", "2", "--delays", "3,0"], ["--local", "2", "--max-ms", "4000"], "refuse"),
    "refuse_unequal_0_3": (["--local", "2", "--delays", "0,3"], ["--local", "2", "--max-ms", "4000"], "refuse"),
    "refuse_1_3_delays_3_0_1": (["--local", "1", "--max-ms", "4000"], ["--local", "3", "--delays", "3,0,1"], "refuse_client"),
    "refuse_pred_ge_depth": (["--local", "2", "--pred", "10", "--depth", "10"], ["--local", "2", "--max-ms", "4000"], "refuse"),
    # the stepper's ring is shallower than GekkoNet's window: a real eviction -> the stepper refuses the load, the session ends, the peer learns at once
    "ring_too_shallow": (["--local", "2", "--timeout-ms", "30000", "--max-ms", "12000"] + ADV, ["--local", "2", "--stepper-depth", "3", "--timeout-ms", "30000"] + ADV, "shallow"),
    # ENHANCEMENT (replays): both peers record a lossy match ({tmp} = the scenario's folder); each recording must play back with every replayed tick's hash equal to
    # the live run's, a state flipped at tick 300 must be reported at exactly 300, and broken files must be refused
    # ENHANCEMENT (rollback latency A/B): the same lossy matches with GekkoNet's limited (sparse) saving: equal hashes, rollbacks, a planted cheat found by the health checks
    "limited_adv_20pct_loss": (["--local", "2", "--limited", "1"] + ADV, ["--local", "2", "--limited", "1"] + ADV, "agree_rb"),
    "limited_split_1_3": (["--local", "1", "--limited", "1"] + ADV, ["--local", "3", "--limited", "1"] + ADV, "agree_rb"),
    "limited_blackout_resume": (["--local", "2", "--limited", "1", "--blackout", "1000,2500", "--timeout-ms", "8000"], ["--local", "2", "--limited", "1", "--blackout", "1000,2500", "--timeout-ms", "8000"], "agree_rb"),
    "limited_cheat": (["--local", "2", "--limited", "1"], ["--local", "2", "--limited", "1", "--cheat", "300"], "cheat_limited"),
    "limited_record_replay": (["--local", "1", "--limited", "1", "--record", "{tmp}/h.gkrp"] + ADV, ["--local", "1", "--limited", "1", "--record", "{tmp}/c.gkrp"] + ADV, "replay"),
    "record_replay": (["--local", "1", "--record", "{tmp}/h.gkrp"] + ADV, ["--local", "1", "--record", "{tmp}/c.gkrp"] + ADV, "replay"),
    # ENHANCEMENT (rollback latency): saves of frames below GekkoNet's final frame take no snapshot (--skip-final 1; the fake ring ERASES those ticks, so a Load of
    # one ends the run). Same results as the full-saving runs: equal hashes under loss and blackouts, the cheat found at exactly 300, replays equal; and some saves skipped
    "skip_final_adv_20pct_loss": (["--local", "2", "--skip-final", "1"] + ADV, ["--local", "2", "--skip-final", "1"] + ADV, "agree_rb"),
    "skip_final_delay3": (["--local", "2", "--skip-final", "1", "--delays", "3,3"] + ADV, ["--local", "2", "--skip-final", "1", "--delays", "3,3"] + ADV, "agree_rb"),
    "skip_final_split_1_3": (["--local", "1", "--skip-final", "1"] + ADV, ["--local", "3", "--skip-final", "1"] + ADV, "agree_rb"),
    "skip_final_blackout_resume": (["--local", "2", "--skip-final", "1", "--blackout", "1000,2500", "--timeout-ms", "8000"], ["--local", "2", "--skip-final", "1", "--blackout", "1000,2500", "--timeout-ms", "8000"], "agree_rb"),
    "skip_final_cheat": (["--local", "2", "--skip-final", "1"], ["--local", "2", "--skip-final", "1", "--cheat", "300"], "cheat"),
    "skip_final_record_replay": (["--local", "1", "--skip-final", "1", "--record", "{tmp}/h.gkrp"] + ADV, ["--local", "1", "--skip-final", "1", "--record", "{tmp}/c.gkrp"] + ADV, "replay"),
    # ENHANCEMENT (lobby): three matches in a row in ONE process per peer, on the same NetSession object, socket and message channel: start, stop, start, stop,
    # start (NetSession::stop and the GekkoNet adapter singleton rebinding). Every round must agree like impaired_2x2 (equal hashes, rollbacks, no desync).
    "restart_3_rounds": (["--local", "2", "--rounds", "3"] + IMP, ["--local", "2", "--rounds", "3"] + IMP, "agree_rounds"),
    # FIX (review of PR 248, jsp_net_ko_rule.h): the match end rules. The host stops pressing (--idle-from 150: the all-zero input GekkoNet also fills in for a
    # gone peer, so no rollback corrects the client's predictions of it) and goes silent at tick 200 (--mute-at); the client predicts up to its window and
    # times out (1.5 s). ko_predicted_disconnect: every tick from 206 is a KO, so the client has saved predicted KOs above anything the host ever sent;
    # they must NOT survive the disconnect (the old rule ended "Match over" on them). ko_confirmed_disconnect (control, the match does not end on the KO
    # itself: --ko-scan 0): the KO of tick 150 was confirmed by the host's real inputs, so the disconnect keeps it. ko_scan_agree: no disconnect, a lossy
    # link with rollbacks, KOs on 1 state in 7 from tick 300: both copies end on the same confirmed tick.
    "ko_predicted_disconnect": (["--local", "2", "--idle-from", "150", "--mute-at", "200", "--timeout-ms", "1500", "--max-ms", "8000"],
                                ["--local", "2", "--ko-from", "206", "--timeout-ms", "1500"], "ko_predicted"),
    "ko_confirmed_disconnect": (["--local", "2", "--idle-from", "150", "--mute-at", "200", "--timeout-ms", "1500", "--max-ms", "8000"],
                                ["--local", "2", "--ko-from", "150", "--ko-scan", "0", "--timeout-ms", "1500"], "ko_confirmed"),
    # The live watermark is the final frame read WITH the events (one session lock). ko_disconnect_gap: as ko_predicted_disconnect, but the client's
    # game thread stops for 2 s between taking a pass's events and using their frame, just after the host went silent; the keepalive thread polls in
    # that gap and times the host out there. The predicted KOs must still not survive, and the watermark must be the frame read with the events.
    "ko_disconnect_gap": (["--local", "2", "--idle-from", "150", "--mute-at", "200", "--timeout-ms", "1500", "--max-ms", "8000"],
                          ["--local", "2", "--ko-from", "206", "--timeout-ms", "1500", "--gap-at", "200,2000"], "ko_gap"),
    "ko_scan_agree": (["--local", "2", "--ko-from", "300", "--ko-every", "7"] + ADV, ["--local", "2", "--ko-from", "300", "--ko-every", "7"] + ADV, "ko_agree"),
    # ENHANCEMENT (slow-link start, jsp_net_delay_rule.h): --adapt 1 runs the game's delay rule on the session's own stats, start delay 2 (the manifest's).
    # The line is 100 ms MEASURED (the answer wait is on top of the link). 1v1 at 60 ms each way (measured about 125 ms): both copies go to 3 within 2 s of
    # the sync; 30 ms each way (measured about 65-75) stays at 2. one_side_2x2: two local players each, only the host adapts (3, on both its slots), the
    # client keeps 2, as two PCs on either side of the line would: the per-side asymmetry the rule already makes. Every row must agree tick by tick.
    # 5 ms loops keep the three rows short (the suite's 600 s limit); 40 ms, the off switch and the rest are in net_delay_rule_test.
    "slow_start_1v1_120ms": (["--local", "1", "--delays", "2", "--adapt", "1", "--delay", "60", "--loss", "5", "--tick-ms", "5"], ["--local", "1", "--delays", "2", "--adapt", "1", "--delay", "60", "--loss", "5", "--tick-ms", "5"], "adapt_slow"),
    "slow_start_1v1_60ms": (["--local", "1", "--delays", "2", "--adapt", "1", "--delay", "30", "--loss", "5", "--tick-ms", "5"], ["--local", "1", "--delays", "2", "--adapt", "1", "--delay", "30", "--loss", "5", "--tick-ms", "5"], "adapt_fast"),
    "slow_start_one_side_2x2": (["--local", "2", "--delays", "2,2", "--adapt", "1", "--delay", "60", "--tick-ms", "5"], ["--local", "2", "--delays", "2,2", "--delay", "60", "--tick-ms", "5"], "adapt_one_side"),
}
KO_KINDS = ("ko_predicted", "ko_confirmed", "ko_agree", "ko_gap")
ADAPT_KINDS = ("adapt_slow", "adapt_fast", "adapt_one_side")


def adapt_check(kind, ho, co):
    """ENHANCEMENT (slow-link start): the "ADAPT" / "ADAPTED" lines of net_session_test --adapt 1 (the hashes are checked as "agree")."""
    f = []
    kv = lambda line: dict(x.split("=", 1) for x in line.split()[1:] if "=" in x)
    slow = lambda out: [kv(l) for l in out.splitlines() if l.startswith("ADAPT ") and "why=slow" in l]
    done = lambda out: next((kv(l) for l in out.splitlines() if l.startswith("ADAPTED ")), None)
    adapting = (("host", ho), ("client", co)) if kind != "adapt_one_side" else (("host", ho),)
    for who, out in adapting:
        d, sl = done(out), slow(out)
        if d is None: f.append(f"{who}: no ADAPTED line"); continue
        # ENHANCEMENT (worst remote): with one remote PC the worst remote is the first one, with the same numbers (rtt_seq tagged with its slot)
        ws, rs = int(d["worst_rtt_seq"]), int(d["rtt_seq"])
        if (d["worst_ping"], d["worst_last_ping"]) != (d["ping"], d["last_ping"]) or d["worst_slot"] != d["first_remote"] or ws & 0xFFFFFF != rs or (ws >> 24) != int(d["first_remote"]) + 1:
            f.append(f"{who}: the worst remote is not the one remote: " + " ".join(f"{k}={d[k]}" for k in ("ping", "worst_ping", "last_ping", "worst_last_ping", "rtt_seq", "worst_rtt_seq", "worst_slot", "first_remote")))
        if int(d["taken"]) < 5: f.append(f"{who}: only {d['taken']} round trips entered the window")
        if kind in ("adapt_slow", "adapt_one_side"):
            if len(sl) != 1: f.append(f"{who}: {len(sl)} slow-link start lines, expected 1"); continue
            if (sl[0]["from"], sl[0]["to"]) != ("2", "3"): f.append(f"{who}: slow-link start {sl[0]['from']} -> {sl[0]['to']}, expected 2 -> 3")
            if int(sl[0]["at"]) > 2100: f.append(f"{who}: slow-link start {sl[0]['at']} ms after the sync, expected by 2 s")
            if d["slow_state"] != "1" or float(d["slow_min"]) < 100: f.append(f"{who}: slow_state={d['slow_state']} slow_min={d['slow_min']}, expected 1 and >= 100")
        elif kind == "adapt_fast":
            if sl: f.append(f"{who}: a slow-link start on a fast link: {sl}")
            if d["slow_state"] != "1" or not (0 < float(d["slow_min"]) < 100): f.append(f"{who}: slow_state={d['slow_state']} slow_min={d['slow_min']}, expected 1 and under 100")
            if d["delay"] != "2": f.append(f"{who}: delay {d['delay']} at the end, expected 2 (the start value)")
    if kind == "adapt_one_side":
        if "ADAPT" in co: f.append("client: adapted without --adapt")
    return f


def owner_check(ho, co, hs, cs, tmp, T):
    """FIX (review of PR 248): stall_owner. The host's game thread is away 7 s (from tick 195); its owner thread ends the session 1 s in. The client must
    see the disconnect about 1 s after its last new tick (well inside the 7 s stall and before its own 5 s timeout), then run on to the end alone."""
    f = []
    if "STALL-ABORT" not in ho: f.append("host: the owner thread did not end the session")
    if "frame clock stalled" not in hs: f.append("host: the session did not fail with the owner's reason: " + hs[-160:])
    m = re.search(r"DISCONNECT-EVENT (\d+) ms after", co)
    if not m: f.append("client: no disconnect event: " + cs[-200:])
    elif not (500 <= int(m.group(1)) <= 3500): f.append(f"client: the disconnect came {m.group(1)} ms after its last new tick, expected about 1000 (the owner's abort, not the 5 s timeout)")
    if num(cs, "maxtick", 0) < T: f.append(f"client: stopped at tick {num(cs, 'maxtick', 0)} of {T} after the host left")
    if num(cs, "tail_neutral", 0) != 1: f.append("client: the gone host's slots do not read neutral")
    n, bad = agree(tmp, 190)
    if bad: f.append(f"hashes differ before the stall: {bad}")
    return f


def ko_check(kind, ho, co):
    """FIX (review of PR 248): the "KO disconnect" / "KO end" lines of net_session_test (--ko-from) against the rule of jsp_net_ko_rule.h."""
    f = []
    kv = lambda line: dict(x.split("=", 1) for x in line.split() if "=" in x)
    disc = lambda out: [kv(l) for l in out.splitlines() if l.startswith("KO disconnect:")]
    ends = lambda out: [kv(l) for l in out.splitlines() if l.startswith("KO end:")]
    gap = lambda out: [kv(l) for l in out.splitlines() if l.startswith("GAP ")]
    if kind == "ko_gap":
        g, d = gap(co), disc(co)
        if len(g) != 1: return [f"client: {len(g)} 'GAP' lines, expected 1"]
        if len(d) != 1: return [f"client: {len(d)} 'KO disconnect' lines, expected exactly 1"]
        g, d = g[0], d[0]
        if int(g["polls"]) < 1: f.append(f"client: no keepalive poll in the {g['ms']} ms gap: the scenario tested nothing")
        if d["result"] != "disconnect": f.append(f"client: a predicted KO (tick {d['at']}) became a match result on the disconnect (watermark {d['watermark']})")
        if int(d["watermark"]) > int(g["events_ff"]) and int(d["watermark"]) > 205: f.append(f"client: watermark {d['watermark']} above the frame read with the events ({g['events_ff']})")
        if ends(co): f.append("client: ended on a KO before the disconnect: " + str(ends(co)))
        return f
    if kind in ("ko_predicted", "ko_confirmed"):
        d = disc(co)
        if len(d) != 1: return [f"client: {len(d)} 'KO disconnect' lines, expected exactly 1 (the host went silent at 200 and must time out)"]
        d = d[0]
        if ends(co): f.append("client: ended on a KO before the disconnect: " + str(ends(co)))
        wm = int(d["watermark"])
        if wm < 150 or wm > 205: f.append(f"client: live watermark {wm}, expected about the host's last sent frame (200..203)")
        if int(d["final_frame"]) < wm: f.append(f"client: final frame {d['final_frame']} below the live watermark {wm}")
        if kind == "ko_predicted":
            if d["result"] != "disconnect": f.append(f"client: a predicted KO (tick {d['at']}) became a match result on the disconnect (watermark {wm})")
            if d["old_rule"] != "match" or int(d["uncovered"]) < 1: f.append(f"no predicted KO was saved before the disconnect (old_rule={d['old_rule']} uncovered={d['uncovered']}): the scenario tested nothing")
            if int(d["old_at"]) < 206: f.append(f"a KO below --ko-from: {d['old_at']}")
        else:
            if d["result"] != "match" or d["at"] != "150": f.append(f"client: the confirmed KO of tick 150 did not survive the disconnect: result={d['result']} at={d['at']} (watermark {wm})")
    elif kind == "ko_agree":
        eh, ec = ends(ho), ends(co)
        if len(eh) != 1 or len(ec) != 1: f.append(f"'KO end' lines: host {len(eh)}, client {len(ec)}, expected 1 each")
        elif eh[0]["at"] != ec[0]["at"]: f.append(f"the copies ended on different ticks: host {eh[0]['at']}, client {ec[0]['at']}")
        elif int(eh[0]["at"]) < 300: f.append(f"ended at {eh[0]['at']}, below --ko-from 300")
        if disc(ho) or disc(co): f.append("a disconnect in a clean match")
    return f
ROUNDS_EXPECTED = {"restart_3_rounds": 3}

# ENHANCEMENT (4-player online): N processes (3 or 4: host + clients, --local 1 each) in a GekkoNet mesh; every process has its own simulated link (its sends:
# loss / delay / jitter, as JSP_NET_LINK does in the game). name: (args per process, host first, kind, extra). Every kind checks equal hashes across ALL processes.
L1, L2, L3 = ["--loss", "50", "--jitter", "20", "--delay", "10"], ["--loss", "100", "--jitter", "30", "--delay", "15"], ["--loss", "20", "--jitter", "10", "--delay", "5"]
MESH = {
    "mesh3_clean": ([["--peers", "3"], [], []], "mesh_agree", {}),
    "mesh4_impaired": ([["--peers", "4"] + L1, L2, L3, ADV], "mesh_agree_rb", {}),
    # each PC claims its battle slot (reverse of the join order); the plan must give exactly those slots, every peer the same roster masks
    "mesh4_claims": ([["--peers", "4", "--claim", "3"] + L1, ["--claim", "2"] + L2, ["--claim", "0"] + L3, ["--claim", "1"]], "mesh_agree", {"slots": [3, 2, 0, 1]}),
    # two clients claim slot 1: the host refuses and every process fails the rendezvous (exit 2) naming the clash
    "mesh4_clash": ([["--peers", "4", "--max-ms", "4000"], ["--claim", "1"], ["--claim", "1"], []], "mesh_clash", {}),
    # one of four is killed mid-match (no goodbye): the three others see its disconnect once, agree on its neutral input from the same frame, and play on to the end
    "mesh4_kill": ([["--peers", "4", "--timeout-ms", "1500"] + L1, ["--timeout-ms", "1500"] + L2, ["--timeout-ms", "1500", "--die", "200"] + L3, ["--timeout-ms", "1500"]], "mesh_gone", {"gone": 2}),
    "mesh3_kill_host": ([["--peers", "3", "--timeout-ms", "1500", "--die", "250"] + L1, ["--timeout-ms", "1500"] + L2, ["--timeout-ms", "1500"]], "mesh_gone", {"gone": 0}),
    "mesh3_leave": ([["--peers", "3"], ["--leave", "200"], []], "mesh_gone", {"gone": 1}),
    # one peer's state flipped at tick 300: every peer reports exactly tick 300
    "mesh4_cheat": ([["--peers", "4"], [], ["--cheat", "300"], []], "mesh_cheat", {}),
    # ENHANCEMENT (review: one broken link): the A-D link (slots 0 and 3) is cut at tick 200, every other link stays up. GekkoNet's disconnect claims make the
    # peers disagree on who is in; the leaver rule must end the match on EVERY process ("CONNECTION LOST"), never play on split. Hashes equal up to the cut.
    "mesh4_cut": ([["--peers", "4", "--claim", "0", "--timeout-ms", "1500", "--cut", "3,200"], ["--claim", "1", "--timeout-ms", "1500"], ["--claim", "2", "--timeout-ms", "1500"],
                   ["--claim", "3", "--timeout-ms", "1500", "--cut", "0,200"]], "mesh_cut", {"cut": 200}),
    # one far peer (60 ms one way on its sends): its link is the worst on every other peer (per-slot ping) and on itself
    "mesh4_far_peer": ([["--peers", "4"], [], [], ["--delay", "60"]], "mesh_ping", {"far": 3}),
    # ENHANCEMENT (4-player online): a 2v2 of four PCs, each claiming its battle slot, every link different (loss 3-10 %, jitter 10-40 ms, one way 5-70 ms) and every PC on
    # its own input delay (1, 2, 3, 2): equal hashes everywhere, the claimed slots, rollbacks, and each slot's input from its own PC only (OWN)
    "mesh4_2v2_asym": ([["--peers", "4", "--claim", "0", "--loss", "30", "--jitter", "10", "--delay", "5", "--delays", "1"],
                        ["--claim", "1", "--loss", "80", "--jitter", "25", "--delay", "20", "--delays", "2"],
                        ["--claim", "2", "--loss", "50", "--jitter", "40", "--delay", "45", "--delays", "3"],
                        ["--claim", "3", "--loss", "100", "--jitter", "15", "--delay", "70", "--delays", "2"]], "mesh_agree_rb", {"slots": [0, 1, 2, 3]}),
    # ENHANCEMENT (4-player online): a peer killed very early, and one killed late: the others play on to the end either way
    "mesh4_kill_early": ([["--peers", "4", "--timeout-ms", "1500"] + L1, ["--timeout-ms", "1500", "--die", "40"] + L2, ["--timeout-ms", "1500"] + L3, ["--timeout-ms", "1500"]], "mesh_gone", {"gone": 1}),
    # ENHANCEMENT (4-player online): two of four killed (no goodbye): the two left must END (the leaver rule: a second player gone ends the match), never hang or play on split
    "mesh4_kill2": ([["--peers", "4", "--timeout-ms", "1500"] + L1, ["--timeout-ms", "1500", "--die", "200"] + L2, ["--timeout-ms", "1500"] + L3, ["--timeout-ms", "1500", "--die", "230"]],
                    "mesh_lost", {"dead": [1, 3], "upto": 180}),
    # ENHANCEMENT (4-player online): one PC dies between the plan and the match (a game killed in the loading): every other process gives up (exit 2), none hangs
    # ENHANCEMENT (4-player online, ownership mutation): PC 2 sends slot 3's pads for its own slot. The hashes still agree everywhere (every PC simulates the same inputs); the OWN
    # check must see the swap on every PC. Proves the ownership check catches what a hash comparison misses.
    "mesh4_swapped": ([["--peers", "4", "--claim", "0"], ["--claim", "1"], ["--claim", "2", "--own-mutate", "1"], ["--claim", "3"]], "mesh_swapped", {"slots": [0, 1, 2, 3]}),
    "mesh4_die_start": ([["--peers", "4"], [], ["--die-at-start", "1"], []], "mesh_dead_start", {"dead": 2}),
}


def run_n(exe, argvs, tmp, deadline_s=DEADLINE_S):
    """Starts the host (argvs[0]) and then every client against its port. Returns ([rc], [output], problems)."""
    end = time.monotonic() + deadline_s
    left = lambda: max(0.0, end - time.monotonic())
    procs, problems = [], []
    try:
        h = Proc([exe, "--role", "host", "--out", str(tmp / "p0.log")] + argvs[0]); procs.append(h)
        h.port_or_eof.wait(left())
        port = h.port()
        if port is None: problems.append("host printed no PORT line")
        else:
            for i, a in enumerate(argvs[1:], 1):
                procs.append(Proc([exe, "--role", "client", "--peer", f"127.0.0.1:{port}", "--out", str(tmp / f"p{i}.log")] + a))
        for i, pr in enumerate(procs):
            try: pr.p.wait(timeout=left())
            except subprocess.TimeoutExpired: problems.append(f"hung: process {i} was still running at the {deadline_s} s scenario deadline (killed)")
    finally:
        for pr in procs:
            if pr.p.poll() is None: pr.p.kill()
            pr.p.wait(); pr.t.join(10)
    return [p.p.returncode for p in procs], [p.out() for p in procs], problems


def mesh_line(out):
    m = [l for l in out.splitlines() if l.startswith("MESH")]
    return m[-1] if m else ""


def check_mesh(kind, rcs, outs, tmp, T, extra):
    f, n = [], len(outs)
    ss, ms = [summary(o) for o in outs], [mesh_line(o) for o in outs]
    if kind == "mesh_clash":
        if any(rc != 2 for rc in rcs): f.append(f"every process must fail the rendezvous (exit 2): {rcs}")
        if not all("clash" in o for o in outs): f.append("not every process names the slot clash")
        return f
    if kind == "mesh_swapped":   # equal hashes AND a swap seen by the OWN check on every process
        for i in range(n):
            if num(own_line(outs[i]), "bad", 0) <= 0: f.append(f"process {i}: the OWN check did not see the swapped pad: {own_line(outs[i])[:160]}")
        lg = {}
        for i in range(n):
            q = tmp / f"p{i}.log"; t = q.read_text().split() if q.exists() else []
            lg[i] = {int(t[k]): t[k + 1] for k in range(0, len(t), 2)}
        for a in range(1, n):
            common = [t for t in lg[0] if t in lg[a] and t <= T - 30]
            if len(common) < T - 40 or any(lg[0][t] != lg[a][t] for t in common): f.append(f"processes 0,{a}: hashes differ or too few ticks (the swap alone must not change them)")
        return f
    if kind == "mesh_dead_start":   # every other process gives up at the barrier (exit 2), the dead one exited 9
        d = extra["dead"]
        if rcs[d] != 9 or "DIED at start" not in outs[d]: f.append(f"process {d} did not die at the start: rc={rcs[d]}")
        for i in range(n):
            if i != d and rcs[i] != 2: f.append(f"process {i}: exit {rcs[i]}, expected 2 (gave up at the barrier): {(ss[i] or outs[i][-200:])[:200]}")
        return f
    if kind == "mesh_lost":   # two of four gone: the others end on the leaver rule; hashes equal before the first death
        dead = extra["dead"]; live_ = [i for i in range(n) if i not in dead]
        for d in dead:
            if rcs[d] != 9 or "DIED" not in outs[d]: f.append(f"process {d} was not killed: rc={rcs[d]}")
        for i in live_:
            if "CONNECTION LOST" not in outs[i]: f.append(f"process {i}: did not end on the leaver rule (no CONNECTION LOST): {(ss[i] or outs[i][-200:])[:200]}")
            if ss[i] and num(ss[i], "desync", -1) != -1: f.append(f"process {i}: a desync was reported: {ss[i][:120]}")
        lg = {}
        for i in range(n):
            q = tmp / f"p{i}.log"; t = q.read_text().split() if q.exists() else []
            lg[i] = {int(t[k]): t[k + 1] for k in range(0, len(t), 2)}
        upto = extra["upto"]
        for a in live_:
            for b in live_:
                if a >= b: continue
                common = [t for t in lg[a] if t in lg[b] and t <= upto]
                if len(common) < upto - 20: f.append(f"processes {a},{b}: only {len(common)} comparable ticks before the first death")
                if any(lg[a][t] != lg[b][t] for t in common): f.append(f"processes {a},{b}: hashes differ before the first death")
        return f
    if kind == "mesh_cut":   # ENHANCEMENT (review): every process ends on the leaver rule, none plays on split; no desync; equal hashes before the cut
        cut = extra["cut"]
        for i in range(n):
            if "CONNECTION LOST" not in outs[i]: f.append(f"process {i}: did not end on the leaver rule (no CONNECTION LOST): {(ss[i] or outs[i][-200:])[:200]}")
            if ss[i] and num(ss[i], "desync", -1) not in (-1,): f.append(f"process {i}: a desync was reported: {ss[i][:120]}")
        lg = {}
        for i in range(n):
            p = tmp / f"p{i}.log"; t = p.read_text().split() if p.exists() else []
            lg[i] = {int(t[k]): t[k + 1] for k in range(0, len(t), 2)}
        for a in range(n):
            for b in range(a + 1, n):
                common = [t for t in lg[a] if t in lg[b] and t <= cut - 20]
                if len(common) < cut - 40: f.append(f"processes {a},{b}: only {len(common)} comparable ticks before the cut")
                if any(lg[a][t] != lg[b][t] for t in common): f.append(f"processes {a},{b}: hashes differ before the cut")
        return f
    gone = extra.get("gone")
    live = [i for i in range(n) if i != gone]   # the leaver stops early: it is not compared over the whole run
    slots = {}
    for i in range(n):
        m = re.search(r"slots=(\d+)\.\.(\d+)/(\d+)", ss[i] or next((l for l in outs[i].splitlines() if l.startswith("PLAN ")), ""))
        if m: slots[i] = int(m.group(1))
        if not m or int(m.group(3)) != n: f.append(f"process {i}: no plan for {n} players: {ss[i][:120] or outs[i][-200:]}")
    if sorted(slots.values()) != list(range(n)) and len(slots) == n: f.append(f"slots are not a permutation: {slots}")
    if "slots" in extra and [slots.get(i) for i in range(n)] != extra["slots"]: f.append(f"claimed slots {extra['slots']}, plan gave {slots}")
    masks = {re.search(r"masks=(\S+)", l).group(1) for l in ms if l}
    if len(masks) != 1: f.append(f"the peers disagree on the roster masks: {masks}")
    for i in live:
        if not re.search(r"roster=%x\b" % ((1 << n) - 1), ms[i]): f.append(f"process {i}: roster mask is not all {n} peers: {ms[i][:120]}")
    logs_ = {i: {} for i in range(n)}
    for i in range(n):
        p = tmp / f"p{i}.log"; t = p.read_text().split() if p.exists() else []
        logs_[i] = {int(t[k]): t[k + 1] for k in range(0, len(t), 2)}
    for i in live:
        if not logs_[i]: f.append(f"process {i}: tick log missing or empty")
    upto = 295 if kind == "mesh_cheat" else T - 30
    for a in live:
        for b in live:
            if a >= b: continue
            common = [t for t in logs_[a] if t in logs_[b] and t <= upto]
            bad = [t for t in common if logs_[a][t] != logs_[b][t]]
            if len(common) < upto - 10: f.append(f"processes {a},{b}: only {len(common)} comparable ticks")
            if bad: f.append(f"processes {a},{b}: hashes differ at ticks {bad[:3]}")
    f += own_check(outs, slots, live, T, extra.get("gone"))
    if kind == "mesh_cheat":
        for i in range(n):
            if num(ss[i], "desync", -1) != 300: f.append(f"process {i}: first desync tick {num(ss[i], 'desync', -1)}, expected exactly 300")
        return f
    for i in live:
        s = ss[i]
        if rcs[i]: f.append(f"process {i}: exit code {rcs[i]}")
        if num(s, "ref_bad", 1) != 0 or num(s, "ref_ticks", 0) < T: f.append(f"process {i}: rollback run differs from straight replay: " + s[:120])
        if num(s, "desync", 0) != -1: f.append(f"process {i}: unexpected desync: " + s[:100])
        if "failed=-" not in s: f.append(f"process {i}: session failed: " + s[:160])
        if num(s, "maxtick", 0) < T: f.append(f"process {i}: stopped at tick {num(s, 'maxtick', 0)} of {T}")
        if kind == "mesh_agree_rb" and num(s, "rollbacks", 0) <= 0: f.append(f"process {i}: no rollbacks happened")
        f += once_per_handle(s, f"process {i}", n - 1)
    if kind == "mesh_gone":
        g = slots.get(gone, -1)
        if "--leave" in MESH_ARGS[gone]:
            if rcs[gone]: f.append(f"leaver exit code {rcs[gone]}")
        elif rcs[gone] != 9 or "DIED" not in outs[gone]: f.append(f"process {gone} was not killed mid-match: rc={rcs[gone]}")
        for i in live:
            d = re.findall(r"disconnected\(h=(\d+)", ss[i])
            if d != [str(g)]: f.append(f"process {i}: disconnect events {d}, expected exactly one for slot {g}")
            if num(ss[i], "tail_neutral", 0) != 1: f.append(f"process {i}: the leaver's slot does not read neutral at the end")
            if not re.search(r"gone=%x\b" % (1 << g), ms[i]) or num(ms[i], "remotes_connected", -1) != n - 2: f.append(f"process {i}: gone mask / connected peers wrong: {ms[i][:160]}")
    if kind == "mesh_ping":
        far = extra["far"]; fs = slots.get(far, -1)
        for i in range(n):
            pings = [float(x) for x in re.search(r"pings=(\S+)", ms[i]).group(1).split(",")] if ms[i] else []
            w = float(re.search(r"worst_ping=(-?\d+)", ms[i]).group(1)) if ms[i] else -1
            if w < 50: f.append(f"process {i}: worst ping {w} ms, expected >= 50 (a 60 ms one-way link)")
            if i != far and pings and max(range(len(pings)), key=lambda k: pings[k]) != fs: f.append(f"process {i}: the worst slot is not the far peer's slot {fs}: {pings}")
    return f


MESH_ARGS = []   # the scenario's per-process args (check_mesh reads it)


def own_line(out):
    m = [l for l in out.splitlines() if l.startswith("OWN")]
    return m[-1] if m else ""


def own_check(outs, slots, live, T, gone=None):
    """ENHANCEMENT (4-player online, ownership: "own pad -> own fighter"): on every live process no slot ever carried another slot's pad (bad=0), and every live slot's pad
    came from its own PC on (nearly) every tick (the first ticks before the first input and a leaver's neutral tail are the only "none")."""
    f = []
    gs = slots.get(gone, -1) if gone is not None else -1
    for i in live:
        o = own_line(outs[i])
        if not o: f.append(f"process {i}: no OWN line"); continue
        if num(o, "bad", 1) != 0: f.append(f"process {i}: a slot carried another slot's pad (SWAPPED / mixed): {o[:160]}")
        ok = [int(x) for x in re.search(r"ok=(\S+)", o).group(1).split(",")]
        for p in sorted(slots.values()):
            if p == gs: continue
            if ok[p] < T - 30: f.append(f"process {i}: slot {p} carried its own PC's pad on only {ok[p]} ticks: {o[:160]}")
        if gs >= 0 and ok[gs] <= 0: f.append(f"process {i}: the leaver's slot {gs} never carried its pad")
    return f



def rounds_check(name, ho, co, T):
    """agree_rounds: every round's SUMMARY of both peers is there, in order, and clean (the last round also had the full agree_rb check on its tick logs)."""
    f, n = [], ROUNDS_EXPECTED[name]
    for who, out in (("host", ho), ("client", co)):
        lines = [l for l in out.splitlines() if l.startswith("SUMMARY")]
        got = [num(l, "round") for l in lines]
        if got != list(range(1, n + 1)): f.append(f"{who}: rounds {got}, expected 1..{n}")
        for l in lines:
            if "failed=-" not in l or num(l, "desync", 0) != -1 or num(l, "ref_bad", 1) != 0: f.append(f"{who}: a round did not run clean: " + l[:200])
            if num(l, "maxtick", 0) < T: f.append(f"{who}: a round stopped early: " + l[:200])
            f += once_per_handle(l, who, 2)
    return f


def once_per_handle(s, who, remotes, kinds=("connected",)):
    """Each remote handle's Connected (and Disconnected, where expected) session event reaches the game exactly once (the update-side event index)."""
    out = []
    m = re.search(r"slots=(\d+)\.\.(\d+)/(\d+)", s)
    if m: remotes = int(m.group(3)) - (int(m.group(2)) - int(m.group(1)) + 1)   # the other peer's slots (split scenarios differ)
    for k in kinds:
        n = len(re.findall(r"(?<![a-z])" + k + r"\(h=", s))
        if n != remotes: out.append(f"{who}: {n} {k} events, expected exactly {remotes} (one per remote slot)")
    return out


def check(kind, hs, cs, hr, cr, tmp, T):
    if kind in ADAPT_KINDS: return check("agree", hs, cs, hr, cr, tmp, T)   # the ADAPT lines: adapt_check (main)
    f = []
    if kind in LOG_KINDS:
        f += [f"{who}: tick log {log} missing or empty (each process writes it at the end of its run; the hash checks need both)"
              for who, log, d in zip(("host", "client"), ("h.log", "c.log"), logs(tmp)) if not d]
    if kind in ("agree", "agree_rb", "agree_paced", "busy_ok"):
        for who, s in (("host", hs), ("client", cs)): f += once_per_handle(s, who, 2)
    if kind in ("agree", "agree_rb", "agree_paced"):
        if hr or cr: f.append(f"exit codes host={hr} client={cr}")
        n, bad = agree(tmp, T - 30)
        if n < T - 40: f.append(f"only {n} comparable ticks")
        if bad: f.append(f"hashes differ at ticks {bad}")
        for s in (hs, cs):
            if num(s, "ref_bad", 1) != 0 or num(s, "ref_ticks", 0) < T: f.append("rollback run differs from straight replay: " + s[:120])
            if num(s, "desync", 0) != -1: f.append("unexpected desync: " + s[:100])
            if "failed=-" not in s: f.append("session failed: " + s[:160])
        if kind == "agree_paced":
            for n, s in (("host", hs), ("client", cs)):
                it, pm = num(s, "iters", 1), num(s, "paced_ms", 0)
                if num(s, "maxdepth", 0) > 10: f.append(f"{n}: rollback depth {num(s, 'maxdepth', 0)} above the prediction window 10")
                if num(s, "stalls", 0) > 0.5 * it: f.append(f"{n}: {num(s, 'stalls', 0)} stalls over {it} iterations (pacing starves the peer)")
                if pm > 0.05 * it * 6: f.append(f"{n}: {pm} ms of pacing sleep over {it} iterations of 6 ms (more than 5% of the run: pacing runaway; a correct 1.6% stretch measured 43-79 ms, broken variants 624-1956 ms)")
        if kind == "agree_rb" and not (num(hs, "rollbacks", 0) > 0 and num(cs, "rollbacks", 0) > 0): f.append("no rollbacks happened")
    elif kind == "busy_ok":
        if hr or cr: f.append(f"exit codes host={hr} client={cr}")
        for n, s in (("host", hs), ("client", cs)):
            if "failed=-" not in s or "disconnected" in s: f.append(f"{n}: the peer timed out or the session failed: " + s[-200:])
            if num(s, "maxtick", 0) < T: f.append(f"{n}: stopped at tick {num(s, 'maxtick', 0)} of {T}")
        if num(hs, "keepalive_polls", 0) < 10: f.append("host: the keepalive thread polled " + str(num(hs, "keepalive_polls", 0)) + " times in a 7 s stall")
        n, bad = agree(tmp, T - 30)
        if bad: f.append(f"hashes differ at ticks {bad}")
    elif kind == "busy_dies":
        if "disconnected" not in cs: f.append("control: the client did not time out without the keepalive thread: " + cs[-200:])
    elif kind in ("rate", "rate_all"):
        if hr or cr: f.append(f"exit codes host={hr} client={cr} (7 = a rate below its bound)")
        for n, s in (("host", hs), ("client", cs)):
            m = re.search(r"\ball_rate=([0-9.]+)", s)
            if not m or float(m.group(1)) < 0.95: f.append(f"{n}: runs per iteration {m.group(1) if m else '?'}, expected >= 0.95")
            if kind == "rate":
                m = re.search(r"\brate=([0-9.]+)", s)
                if not m or float(m.group(1)) < 0.9: f.append(f"{n}: live ticks per iteration {m.group(1) if m else '?'}, expected >= 0.9 (sparse inputs)")
    elif kind == "cheat":
        # cheat at tick 300 changes the state after tick 300 = the state Save(299) takes: reported in stepper tick numbering (Save frame + 1), exactly tick 300
        for n, s in (("host", hs), ("client", cs)):
            d = num(s, "desync", -1)
            if d != 300: f.append(f"{n}: first desync tick {d}, expected exactly 300")
        n, bad = agree(tmp, 295)
        if bad: f.append(f"hashes differ before the cheat: {bad}")
    elif kind == "cheat_limited":
        # limited saving: desync detection runs on GekkoNet's health frames (every 16 ticks), so the cheat at tick 300 is reported at the first health tick at or after it
        for n, s in (("host", hs), ("client", cs)):
            d = num(s, "desync", -1)
            if not (300 <= d <= 300 + 17): f.append(f"{n}: first desync tick {d}, expected 300..317 (the first health frame after the cheat)")
        n, bad = agree(tmp, 295)
        if bad: f.append(f"hashes differ before the cheat: {bad}")
    elif kind == "leave":
        f += once_per_handle(hs, "host", 2, ("connected", "disconnected"))
        if "disconnected" not in hs: f.append("host saw no disconnect: " + hs[-160:])
        if num(hs, "maxtick", 0) < T: f.append("host did not run on to the end after the leave")
        if num(hs, "tail_neutral", 0) != 1: f.append("slots of the peer that left do not read neutral (zero wire bytes) on the host")
        n, bad = agree(tmp, 190)
        if bad: f.append(f"hashes differ before the leave: {bad}")
        if cr: f.append(f"leaver exit code {cr}")
    elif kind == "mismatch":
        if hr != 3 or cr != 3: f.append(f"expected both to refuse (exit 3): host={hr} client={cr}")
    elif kind == "refuse":
        if hr != 6 or "equal" not in hs and "ring depth" not in hs: f.append(f"host should refuse to start (exit 6): exit={hr} {hs[:200]}")
    elif kind == "refuse_client":
        if cr != 6 or "equal" not in cs: f.append(f"client should refuse to start (exit 6): exit={cr} {cs[:200]}")
    elif kind == "replay":
        f += check("agree_rb", hs, cs, hr, cr, tmp, T)
        if not REPLAY_EXE: return f + ["no --replay-exe given"]
        live_h, live_c = logs(tmp)
        for w, live in (("h", live_h), ("c", live_c)):
            rec, out = tmp / f"{w}.gkrp", tmp / f"{w}.replay.log"
            if not rec.exists(): f.append(f"{w}: no recording written"); continue
            p = subprocess.run([REPLAY_EXE, "--mode", "replay", "--replay", str(rec), "--out", str(out)], capture_output=True, text=True, timeout=60)
            if p.returncode: f.append(f"{w}: replay exit {p.returncode}: {p.stdout[-300:]}"); continue
            t = out.read_text().split()
            rp = {int(t[i]): t[i + 1] for i in range(0, len(t), 2)}
            if len(rp) < T - 40: f.append(f"{w}: the replay ran {len(rp)} ticks, expected at least {T - 40}")
            # The last ticks are not compared: the recording holds inputs confirmed by the final network poll, which the live game ended before rolling back
            # to (its log keeps the predicted value). Every tick the live run settled must match.
            settled = max(rp) - 10 if rp else 0
            bad = [k for k in rp if k <= settled and live.get(k) != rp[k]]
            if bad: f.append(f"{w}: replayed hashes differ from the live run at ticks {bad[:3]}")
        for extra in (["--mode", "replay", "--cheat", "300"], ["--mode", "bad-replay"]):
            p = subprocess.run([REPLAY_EXE, "--replay", str(tmp / "h.gkrp")] + extra, capture_output=True, text=True, timeout=60)
            if p.returncode: f.append(f"{' '.join(extra)}: exit {p.returncode}: {p.stdout[-300:]}")
    elif kind == "agree_rounds":   # the last round's logs and SUMMARY as agree_rb; every round's SUMMARY in rounds_check (main)
        f += check("agree_rb", hs, cs, hr, cr, tmp, T)
    elif kind == "shallow":
        if cr != 4 or "rollback-deeper-than-the-ring" not in cs: f.append(f"client should fail on the ring depth: exit={cr} {cs[:220]}")
        if "disconnected" not in hs: f.append("host was not told at once (30 s timeout, no disconnect event): " + hs[-200:])
    return f


# CI: the scenarios run in N parts (net_session_test.sh SCRATCH K/N, one host-tests row each), so no row comes near the runner's 600 s limit per
# test. Round robin over the scenarios in table order; the names in one GROUP (scenarios compared with each other) always run in the same part,
# under the first name's turn. No part (the default, and --only) runs everything, as before.
GROUP = {}


def parse_part(s):
    m = re.fullmatch(r"(\d+)/(\d+)", s or "")
    if not m or not 1 <= int(m.group(1)) <= int(m.group(2)):
        raise SystemExit(f"--part: expected K/N with 1 <= K <= N, got {s!r}")
    return int(m.group(1)), int(m.group(2))


def in_part(name, names, part):
    if not part: return True
    groups = []
    for x in names:
        g = GROUP.get(x, x)
        if g not in groups: groups.append(g)
    return groups.index(GROUP.get(name, name)) % part[1] == part[0] - 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="the net_session_test binary (net_session_test.sh builds it and passes it here)")
    ap.add_argument("--only"); ap.add_argument("--prefix", default="", help="only the scenarios whose name starts with this (e.g. --prefix mesh)"); ap.add_argument("--ticks", type=int, default=600)
    ap.add_argument("--part", help="K/N: only the K-th of N parts of the scenarios (CI runs each part as its own row)")
    ap.add_argument("--deadline", type=float, default=DEADLINE_S, help="seconds per scenario, both processes together (default %(default)s)")
    ap.add_argument("--replay-exe", help="the net_replay_test binary (the record_replay scenario)")
    a = ap.parse_args()
    part = parse_part(a.part) if a.part else None
    names = list(SCENARIOS) + list(MESH)   # ENHANCEMENT (4-player online): the mesh rows are split over the parts too
    global REPLAY_EXE
    REPLAY_EXE = a.replay_exe
    fails = 0
    for name, (ha, ca, kind) in SCENARIOS.items():
        if (a.only and a.only != name) or not name.startswith(a.prefix) or not in_part(name, names, part): continue
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            ex = ["--ticks", str(a.ticks), "--seed", "5"]
            t0 = time.monotonic()
            sub = lambda args: [x.replace("{tmp}", str(tmp)) for x in args]
            hr, ho, cr, co, problems = run(a.exe, sub(ha) + ex, sub(ca) + ex, tmp, a.deadline)
            hs, cs = summary(ho), summary(co)
            fl = problems or check(kind, hs, cs, hr, cr, tmp, a.ticks)
            if not problems and kind == "agree_rounds": fl += rounds_check(name, ho, co, a.ticks)
            if not problems and kind in KO_KINDS: fl += ko_check(kind, ho, co)
            if not problems and kind in ADAPT_KINDS: fl += adapt_check(kind, ho, co)
            if not problems and kind == "stall_owner": fl += owner_check(ho, co, hs, cs, tmp, a.ticks)
            if not fl and name.startswith("skip_final"):   # the switch must have skipped saves on both peers, or the scenario tested nothing
                for n_, s_ in (("host", hs), ("client", cs)):
                    if num(s_, "skipped", 0) <= 0: fl.append(f"{n_}: no save was skipped (skipped={num(s_, 'skipped', -1)})")
            print(f"{'PASS' if not fl else 'FAIL'} {name} ({time.monotonic() - t0:.1f} s)", flush=True)
            for s in (hs, cs): print("   ", s[:330])
            for x in fl: print("   !", x)
            if problems:
                for x in tails(tmp, ho, co): print("   |", x)
            fails += bool(fl)
    global MESH_ARGS
    for name, (argvs, kind, extra) in MESH.items():   # ENHANCEMENT (4-player online)
        if (a.only and a.only != name) or not name.startswith(a.prefix) or not in_part(name, names, part): continue
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            ex = ["--local", "1", "--ticks", str(a.ticks), "--seed", "5"]
            MESH_ARGS = argvs
            t0 = time.monotonic()
            rcs, outs, problems = run_n(a.exe, [x + ex for x in argvs], tmp, a.deadline)
            fl = problems or check_mesh(kind, rcs, outs, tmp, a.ticks, extra)
            print(f"{'PASS' if not fl else 'FAIL'} {name} ({time.monotonic() - t0:.1f} s)", flush=True)
            for o in outs: print("   ", summary(o)[:330]); print("   ", mesh_line(o)[:200]); print("   ", own_line(o)[:200])
            for x in fl: print("   !", x)
            if problems:
                for i, o in enumerate(outs): print(f"   | process {i}:", *o.splitlines()[-6:], sep="\n   |   ")
            fails += bool(fl)
    print("RESULT", "FAIL" if fails else "ok", f"({fails} failed)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
