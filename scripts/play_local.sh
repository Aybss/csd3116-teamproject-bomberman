#!/usr/bin/env bash
# play_local.sh - try the game on one machine.
# Starts a server and several client windows, so you can play against yourself.
# Everything stops when you close the last window.
#
#   ./scripts/play_local.sh            # 2 players, no security
#   ./scripts/play_local.sh 3 --secure # 3 players, DDS Security on
set -euo pipefail
cd "$(dirname "$0")/.."

PLAYERS="${1:-2}"
SECURE="${2:-}"
BIN=./build
[ -x "$BIN/ba_server" ] || { echo "Build first: cmake -B build && cmake --build build"; exit 1; }

sec() {   # $1 = identity. Prints "--secure security/out/$1.xml" when --secure
    if [ "$SECURE" = "--secure" ]; then echo "--secure security/out/$1.xml"; fi
}
if [ "$SECURE" = "--secure" ] && [ ! -f security/out/server.xml ]; then
    ./security/gen_security.sh 5
fi

pids=()
cleanup() { kill -INT "${pids[@]}" 2>/dev/null || true; wait 2>/dev/null || true; }
trap cleanup EXIT

$BIN/ba_server $(sec server) & pids+=($!)
sleep 1
for i in $(seq 2 "$PLAYERS"); do
    $BIN/ba_client --name "Player$i" $(sec "client$i") > /dev/null & pids+=($!)
done
$BIN/ba_client --name "Player1" $(sec client1)
