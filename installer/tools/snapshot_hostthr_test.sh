#!/usr/bin/env bash
# Host test of the GPU driver threads the snapshot freeze leaves running (installer/port/jsp/jsp_snapshot_hostthr.h). No game, no GPU: needs g++ (C++20).
#   bash installer/tools/snapshot_hostthr_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_hostthr_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -Wall -Wextra -Werror -I"$d/../port/jsp" -o "$out" "$d/snapshot_hostthr_test.cpp"
"$out"
