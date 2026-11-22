#!/usr/bin/env bash
# Host test (FIX pick7) of the after-match menu against the per-match rules (installer/tools/net_pick_match_test.cpp): the match-end rule shared by
# the share pad watchdog's flag and the prompt, its late-but-alive stop, the delay rule's start and the [perf] summary across Fight again hand-offs. Then a source guard on
# jsp_net_game.cpp: reset_match resets the delay rule's and the perf summary's state, end_with logs [perf] before it and keeps the order
# flag -> disarm -> prompt on one end rule, and every session start maps the fighters with the planned seats. No game, no GPU: needs g++ (C++20).
#   bash installer/tools/net_pick_match_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_net_pick_match_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -O2 -Wall -Wextra -o "$out" "$d/net_pick_match_test.cpp"
"$out"

src="$d/../port/jsp/jsp_net_game.cpp"
body() { tr -d '\r' < "$src" | awk -v h="$1" 'index($0, h) == 1 { on = 1 } on { print } on && /^}/ { exit }'; }
fail=0
need() { if ! printf '%s\n' "$1" | grep -qF -- "$2"; then echo "source guard: $3 is missing: $2"; fail=1; fi; }
before() {
    local a b
    a=$(printf '%s\n' "$1" | grep -nF -- "$2" | head -1 | cut -d: -f1)
    b=$(printf '%s\n' "$1" | grep -nF -- "$3" | head -1 | cut -d: -f1)
    if [ -z "$a" ] || [ -z "$b" ] || [ "$a" -ge "$b" ]; then echo "source guard: in $4, '$2' must come before '$3'"; fail=1; fi
}
rm_body=$(body "void reset_match()")
need "$rm_body" "g_ad = jpdr::State();" "reset_match (the delay rule's state per match)"
need "$rm_body" "g_perf = jpperf::Match();" "reset_match (the [perf] lines per match)"
ew_body=$(body "void end_with(")
need "$ew_body" "jspo::end_rule(text)" "end_with (one match-end rule)"
before "$ew_body" "perf_log();" "reset_match();" "end_with"
before "$ew_body" "if (er.wd_flag) jsp_native_fight_end_normal();" "jsp_native_fight_arm(0);" "end_with"
before "$ew_body" "jsp_native_fight_arm(0);" "g.flow.match_end(" "end_with"
ss_body=$(body "void start_session(")
need "$ss_body" "jsp_native_fight_map((int)g.plan.local_first, jspo::plan_seats((int)g.plan.total_players)" "start_session (the planned seats)"
pt_body=$(body "void pump_tick(")
need "$pt_body" "jspo::seats_ok(slots, 2, (int)g.plan.total_players)" "pump_tick (the menu's link on the planned seats)"
need "$pt_body" "g.flow.game_alive(t)" "pump_tick (the freeze owner while the menu is up)"
if [ $fail -ne 0 ]; then echo "net pick match: source guard FAILED"; exit 1; fi
echo "net pick match: source guard ok (reset_match, end_with order, planned seats, freeze owner)"
