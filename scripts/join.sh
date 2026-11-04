#!/usr/bin/env bash
# join.sh - start a game client that connects to a host.
#
#   ./scripts/join.sh SERVER_ADDRESS [IDENTITY] [NAME]
#
#   SERVER_ADDRESS  the address host.sh printed
#                   "local" = the server is on this machine
#                   "auto"  = find the server by multicast (same network only)
#   IDENTITY        client1 ... client5. Use a different one on every machine.
#   NAME            name on the scoreboard (default: same as IDENTITY)
#
#   INSECURE=1 ./scripts/join.sh SERVER_ADDRESS    no security (quick tests only)
#   DDS_TRACE=1 ./scripts/join.sh ...              write a debug log to /tmp/dds.log
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/scripts/net_common.sh"

SERVER="${1:-auto}"
ID="${2:-client1}"
NAME="${3:-$ID}"
[ -x build/ba_client ] || { echo "Build first: cmake -B build && cmake --build build"; exit 1; }

if [ "$SERVER" = "local" ]; then SERVER="$(show_ips | head -n1 || true)"; fi
[ -n "$SERVER" ] || { echo "Could not find an address for this machine."; exit 1; }

peers=()
if [ "$SERVER" != "auto" ]; then
    peers=("$SERVER")
    if ! ping -c 2 -W 1 "$SERVER" > /dev/null 2>&1; then
        echo "Warning: cannot ping $SERVER."
        echo "Check: same Wi-Fi? On Windows, run scripts/windows_setup.ps1 as Administrator."
    fi
fi

if [ "${INSECURE:-}" = "1" ]; then
    echo "INSECURE mode: no authentication, no encryption."
    write_config none "$SEC/local_none.xml" ${peers[@]+"${peers[@]}"}
    exec ./build/ba_client --name "$NAME" --secure "$SEC/local_none.xml"
fi

need_files "$ID"
write_config "$ID" "$SEC/local_$ID.xml" ${peers[@]+"${peers[@]}"}
exec ./build/ba_client --name "$NAME" --secure "$SEC/local_$ID.xml"
