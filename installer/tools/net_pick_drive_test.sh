#!/usr/bin/env bash
# Host test of the after-match drive rules (installer/port/jsp/jsp_net_pick_drive.h): the prompt's pad gate, the host's late-pick start block, the
# notice-box Cross (mapping run U4) and the joiner's Ready Cross (U7), plus the map1 timelines on a model room page. No game, no GPU: needs only g++ (C++20).
#   bash installer/tools/net_pick_drive_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_net_pick_drive_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -O2 -Wall -Wextra -o "$out" "$d/net_pick_drive_test.cpp"
"$out"
