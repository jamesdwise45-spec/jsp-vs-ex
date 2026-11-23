#!/usr/bin/env bash
# Host test of the character pick after the rollback post-game prompt (installer/port/jsp/jsp_net_pick.h): wire format, the pick agreement for
# 2..4 players, Random, and the roster write in both team modes (the same bytes on every PC). No game, no GPU: needs only g++ (C++20).
#   bash installer/tools/net_pick_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_net_pick_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -O2 -Wall -Wextra -o "$out" "$d/net_pick_test.cpp"
"$out"
