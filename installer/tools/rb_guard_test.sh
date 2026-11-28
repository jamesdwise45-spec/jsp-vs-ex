#!/usr/bin/env bash
# Host test of the rollback guards' pure rules (installer/port/jsp/jsp_rb_guard.h, FIX test13): the NULL-call loop counter, the desync-first order of a batch of
# session events, the reasons of a skipped helper-thread register write-back and the loader boundary wait's off switch. No game, no GPU: needs only g++ (C++17).
#   bash installer/tools/rb_guard_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_rb_guard_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -O1 -Wall -Wextra -Werror -o "$out" "$d/rb_guard_test.cpp"
"$out"
