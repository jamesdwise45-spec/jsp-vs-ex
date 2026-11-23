#!/usr/bin/env bash
# Host test of Select character on the game's own Battle settings page for every player (installer/port/jsp/jsp_net_pick_page.h, JSP_PICK_NATIVE_SELECT=1).
# The page step, two PCs end to end over the real jspf::Flow, and a source guard on the wiring. No game, no GPU: needs only g++ (C++20).
#   bash installer/tools/net_pick_page_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_net_pick_page_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -O2 -Wall -Wextra -o "$out" "$d/net_pick_page_test.cpp"
"$out" "$d"
