#!/usr/bin/env bash
# Host test of toolkit patch 7936 (no game, no GPU, no network: a fake P2P sender; needs a C compiler and the patched toolkit): the
# toolkit's own libs/network/tests/test_np_nat.c drives np_psnr's real NAT code: probing, punching, the relay after 3 s of silence,
# and (7936) a peer whose router remapped its P2P port, learned from the user id in its punch (same public IP only; the title's
# datagrams go to that port and come back rewritten to the announced address; PSNR_PUNCH_LEARN=0 learns nothing), the relay used
# only when GET_CAPS has bit 5 (a pre-GET_CAPS server: HELLO_ACK's flag; an error or no answer: no relay; PSNR_RELAY_NEEDS_CAPS=0:
# HELLO_ACK's flag), and the per-peer room counters. It sleeps about 4 s (the punch and relay timers are real).
# On Windows (Git bash) it builds with MINGW_CC (e.g. llvm-mingw's x86_64-w64-mingw32-clang), else x86_64-w64-mingw32-gcc or -clang
# on PATH; with none it says SKIP and passes, unless CI=1. Elsewhere it builds with cc (or $CC).
#   bash installer/tools/np_nat_test.sh TOOLKIT_DIR [SCRATCH_DIR]      (TOOLKIT_DIR: installer/tools/export_toolkit.py --out DIR)
set -euo pipefail
tk="${1:?usage: np_nat_test.sh TOOLKIT_DIR [SCRATCH_DIR]}"
src="$tk/libs/network/tests/test_np_nat.c"
[[ -f "$src" ]] || { echo "no $src (is the toolkit exported?)" >&2; exit 3; }
grep -q np_psnr_peer_stats "$tk/libs/network/np_psnr.c" || { echo "$tk/libs/network/np_psnr.c has no np_psnr_peer_stats (is patch 7936 applied?)" >&2; exit 3; }
out="${2:-}"
if [[ -z "$out" ]]; then out=$(mktemp -d "${TMPDIR:-/tmp}/jsp_np_nat_test.XXXXXX"); trap 'rm -rf "$out"' EXIT; fi
mkdir -p "$out"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        cc="${MINGW_CC:-$(command -v x86_64-w64-mingw32-gcc || command -v x86_64-w64-mingw32-clang || true)}"
        if [[ -z "$cc" ]]; then
            [[ "${CI:-}" == 1 ]] && { echo "no Windows C compiler (set MINGW_CC)" >&2; exit 2; }
            echo "SKIP np_nat_test: no Windows C compiler (set MINGW_CC)"; exit 0
        fi
        exe="$out/test_np_nat.exe"
        (cd "$tk" && "$cc" -std=gnu17 -O1 -UNDEBUG -Wall -Iinclude libs/network/tests/test_np_nat.c -lws2_32 -o "$exe")
        bindir=$(dirname "$cc")
        for d in "$bindir/../x86_64-w64-mingw32/bin" "$bindir"; do
            for f in "$d"/libunwind.dll "$d"/libc++.dll "$d"/libwinpthread-1.dll; do [[ -f "$f" ]] && cp -f "$f" "$out/"; done
        done
        log=$(timeout 60 "$exe")
        ;;
    *)
        cc="${CC:-$(command -v cc || command -v gcc || command -v clang)}"
        (cd "$tk" && "$cc" -std=gnu17 -O1 -UNDEBUG -Wall -Iinclude libs/network/tests/test_np_nat.c -pthread -o "$out/test_np_nat")
        log=$(timeout 60 "$out/test_np_nat")
        ;;
esac
printf '%s\n' "$log"
# The log lines the field bundles are read by (JOIN-FAIL.md): the learned port, the relay decision, one counter line per peer.
grep -q 'user 21 punches from port 1369, not its announced 3658' <<<"$log"
grep -q 'the relay needs a relay pass this player does not have' <<<"$log"
grep -q 'user 21, this room: heard directly 1, relayed 0 out / 0 in, punches from another port 1 (last port 1369, announced 3658); path: direct, remapped port' <<<"$log"
grep -q 'test_np_nat: all passed' <<<"$log"
echo "np_nat_test: ok"
