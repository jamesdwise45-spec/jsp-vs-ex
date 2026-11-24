#!/usr/bin/env bash
# Host test of the 4-player online glue rules (installer/port/jsp/jsp_net_four.h) and the mesh factory over real loopback UDP: pairing and re-planning,
# the slot claim from the room records, permuted claims, a clash, the same-house endpoint view, the leaver, the takeover's human slots.
# Native Linux, no game, no GPU; needs g++ (C++20).
#   bash installer/tools/net_four_test.sh [SCRATCH_DIR]
set -euo pipefail
d=$(cd "$(dirname "$0")" && pwd)
port="$d/../port/jsp"
g="$port/gekkonet"
out="${1:-}"
if [[ -z "$out" ]]; then out=$(mktemp -d "${TMPDIR:-/tmp}/jsp_net_four_test.XXXXXX"); trap 'rm -rf "$out"' EXIT; fi
mkdir -p "$out/obj"
common=(-std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -DGEKKONET_STATIC -DGEKKONET_NO_ASIO -I"$g/include" -I"$port")
for f in "$g"/src/*.cpp; do   # third-party, unchanged: compiled without warnings
  g++ "${common[@]}" -I"$g/include/private" -I"$g/thirdparty" -w -c "$f" -o "$out/obj/$(basename "$f" .cpp).o"
done
g++ "${common[@]}" -c "$port/jsp_net_session.cpp" -o "$out/jsp_net_session.o"
g++ "${common[@]}" "$d/net_four_test.cpp" "$out/jsp_net_session.o" "$out"/obj/*.o -o "$out/net_four_test" -pthread
"$out/net_four_test"
