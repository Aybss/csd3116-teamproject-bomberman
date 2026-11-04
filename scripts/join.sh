#!/usr/bin/env bash
# join.sh - start a game client that connects to a host.
#
#   ./scripts/join.sh HOST_ADDRESS JOIN-CODE [NAME]   first time on this machine
#   ./scripts/join.sh HOST_ADDRESS [NAME]             again later (same host)
#   ./scripts/join.sh local [client1] [NAME]          play on the host machine
#
#   HOST_ADDRESS  the address host.sh printed
#                 "local" = the server is on this machine
#                 "auto"  = find the server by multicast (same network only)
#   JOIN-CODE     the code host.sh printed (like K7QX-M2PA). Your machine makes
#                 its own key and gets a certificate from the host with it.
#   NAME          name on the scoreboard
#
#   ./scripts/join.sh HOST_ADDRESS client3 [NAME]     use files from make_bundle.sh
#   INSECURE=1 ./scripts/join.sh HOST_ADDRESS [NAME]  no security (quick tests only)
#   DDS_TRACE=1 ./scripts/join.sh ...                 write a debug log to /tmp/dds.log
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/scripts/net_common.sh"

SERVER="${1:-auto}"
ARG2="${2:-}"
ARG3="${3:-}"
[ -x build/ba_client ] || { echo "Build first: cmake -B build && cmake --build build"; exit 1; }

is_identity() { [[ "$1" =~ ^client[1-5]$ ]]; }
is_code()     { [[ "$1" =~ ^[A-Za-z0-9]{4}-[A-Za-z0-9]{4}$ ]]; }

host_ip="$SERVER"
PRIO=""
if [ "$SERVER" = "local" ]; then
    host_ip="$(show_ips | head -n1 || true)"
    PRIO="nice -n 5"    # on the host, the server goes first
fi
use_gpu
[ -n "$host_ip" ] || { echo "Could not find an address for this machine."; exit 1; }

peers=()
if [ "$host_ip" != "auto" ]; then
    peers=("$host_ip")
    if ! ping -c 2 -W 1 "$host_ip" > /dev/null 2>&1; then
        echo "Warning: cannot ping $host_ip."
        echo "Check: same Wi-Fi? On Windows, run scripts/windows_setup.ps1 as Administrator."
    fi
fi

if [ "${INSECURE:-}" = "1" ]; then
    NAME="${ARG2:-player}"
    echo "INSECURE mode: no authentication, no encryption."
    write_config none "$SEC/local_none.xml" ${peers[@]+"${peers[@]}"}
    exec $PRIO ./build/ba_client --name "$NAME" --secure "$SEC/local_none.xml"
fi

if [ "$SERVER" = "local" ]; then
    # On the host: use the host's own files.
    ID="${ARG2:-client1}"
    is_identity "$ID" || { ARG3="$ID"; ID=client1; }
    NAME="${ARG3:-$ID}"
elif is_code "$ARG2"; then
    # First time: get a certificate from the host with the join code.
    [ "$host_ip" != "auto" ] || { echo "A join code needs the host's address, not 'auto'."; exit 1; }
    KEYDIR="$SEC/join"
    python3 scripts/join_code.py fetch --host "$host_ip" --code "$ARG2" --dir "$KEYDIR"
    ID="$(cat "$KEYDIR/identity.txt")"
    NAME="${ARG3:-$ID}"
elif is_identity "$ARG2"; then
    # Files unpacked from a make_bundle.sh bundle.
    ID="$ARG2"
    NAME="${ARG3:-$ID}"
elif [ -f "$SEC/join/identity.txt" ]; then
    # Joined this host before: reuse that certificate.
    KEYDIR="$SEC/join"
    ID="$(cat "$KEYDIR/identity.txt")"
    NAME="${ARG2:-$ID}"
else
    echo "Ask the host for the join code, then run:"
    echo "  ./scripts/join.sh $SERVER JOIN-CODE YourName"
    exit 1
fi

need_files "$ID"
write_config "$ID" "$SEC/local_$ID.xml" ${peers[@]+"${peers[@]}"}
echo "Joining $host_ip as $NAME ($ID)"
exec $PRIO ./build/ba_client --name "$NAME" --secure "$SEC/local_$ID.xml"
