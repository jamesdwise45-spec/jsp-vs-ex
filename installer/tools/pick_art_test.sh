#!/usr/bin/env bash
# Host test of the after-match menu's game-art reader and painter (installer/port/jsp/jsp_pick_art.h, jsp_pick_skin.h): $CMP, STPK,
# SRD, DXT / Morton decoding, the SpFt glyph table and the drawn fallback of every layout. Synthetic data only: no game, no GPU; needs g++ (C++20).
#   bash installer/tools/pick_art_test.sh
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_pick_art_test.XXXXXX")
trap 'rm -f "$out"' EXIT
g++ -std=c++20 -O2 -Wall -Wextra -o "$out" "$d/pick_art_test.cpp"
"$out"
