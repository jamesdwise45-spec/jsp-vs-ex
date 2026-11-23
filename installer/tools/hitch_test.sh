#!/usr/bin/env bash
# Host test of the per-hitch line (installer/port/jsp/jsp_hitch.h). No game, no GPU: needs g++ (C++20).
#   bash installer/tools/hitch_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_hitch_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -Wall -Wextra -I"$d/../port/jsp" -o "$out" "$d/hitch_test.cpp"
"$out"
