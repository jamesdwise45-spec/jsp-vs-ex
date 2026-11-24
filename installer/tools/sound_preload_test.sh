#!/usr/bin/env bash
# Host test of the sound archive preload (installer/port/jsp/jsp_sound_preload_rule.h): which file, the JSP_SOUND_PRELOAD_MB budget, the RAM cap.
# With TOOLKIT (the patched ps3recomp folder, installer/tools/export_toolkit.py --out DIR) it also builds the glue itself
# (installer/port/jsp/jsp_sound_preload.cpp, its POSIX branch) against ps3emu/fs_vfile.h and runs it on a synthetic file in a temporary folder.
# No game data, no GPU: needs only a C++20 compiler (CXX, default g++).
#   bash installer/tools/sound_preload_test.sh [TOOLKIT]
set -e
d=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp "${TMPDIR:-/tmp}/jsp_sound_preload_test.XXXXXX")
dir=$(mktemp -d "${TMPDIR:-/tmp}/jsp_sound_preload_dir.XXXXXX")
trap 'rm -f "$out"; rm -rf "$dir"' EXIT
if [ -n "${1:-}" ]; then
    "${CXX:-g++}" -std=c++20 -O1 -Wall -Wextra -pthread -DSOUND_PRELOAD_GLUE -I "$d/../port/jsp" -I "$1/include" -o "$out" \
        "$d/sound_preload_test.cpp" "$d/../port/jsp/jsp_sound_preload.cpp"
    "$out" "$dir"
else
    "${CXX:-g++}" -std=c++20 -O1 -Wall -Wextra -I "$d/../port/jsp" -o "$out" "$d/sound_preload_test.cpp"
    "$out"
fi
