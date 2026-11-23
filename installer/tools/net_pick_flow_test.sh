#!/usr/bin/env bash
# Host test of the post-game / pick wiring (installer/port/jsp/jsp_net_pick_flow.h): the 3..4 player choice, the routing of the link's P / C tags and
# the whole flow of N = 2..4 PCs (prompt, Fight again / Select character, the pick round, the host's start, the roster write). No game, no GPU: needs
# only g++ (C++20).
#   bash installer/tools/net_pick_flow_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_net_pick_flow_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -O2 -Wall -Wextra -o "$out" "$d/net_pick_flow_test.cpp"
"$out"
