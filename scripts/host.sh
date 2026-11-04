#!/usr/bin/env bash
# host.sh - start the game server so other machines can join.
#
#   ./scripts/host.sh                      secure server, prints a join code
#   ./scripts/host.sh 10.0.0.5 10.0.0.6    also contact these player addresses
#   INSECURE=1 ./scripts/host.sh           no security (for quick tests only)
#   DDS_TRACE=1 ./scripts/host.sh          write a debug log to /tmp/dds.log
#
# The first time it makes the security files for you. Players get their own
# certificate automatically with the join code (see scripts/join_code.py),
# so nothing has to be copied between machines.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/scripts/net_common.sh"

[ -x build/ba_server ] || { echo "Build first: cmake -B build && cmake --build build"; exit 1; }

# Give the server a higher priority, so a game window on this machine
# cannot slow it down (needs root, which WSL usually is).
PRIO=""
if [ "$(id -u)" = 0 ]; then PRIO="nice -n -10"; fi

ips="$(show_ips)"
first_ip="$(echo "$ips" | head -n1)"
echo "Addresses of this machine:"
echo "$ips" | sed 's/^/  /'
echo

if [ "${INSECURE:-}" = "1" ]; then
    echo "INSECURE mode: no authentication, no encryption."
    echo "Players join with:  INSECURE=1 ./scripts/join.sh ${first_ip:-HOST_ADDRESS}"
    write_config none "$SEC/local_none.xml" "$@"
    exec $PRIO ./build/ba_server --secure "$SEC/local_none.xml"
fi

if [ ! -f "$SEC/server_cert.pem" ] || [ ! -f "$SEC/ca_key.pem" ]; then
    echo "No security files yet, making them..."
    bash ./security/gen_security.sh 5
    echo
fi
need_files server
write_config server "$SEC/local_server.xml" "$@"

# Hand out player certificates while the server runs.
CODE="$(python3 scripts/join_code.py new-code)"
python3 scripts/join_code.py serve --dir "$SEC" --code "$CODE" &
helper=$!
trap 'kill "$helper" 2>/dev/null || true' EXIT

echo "=============================================================="
echo "  Join code:  $CODE"
echo
echo "  Players on other machines run:"
echo "    ./scripts/join.sh ${first_ip:-HOST_ADDRESS} $CODE TheirName"
echo
echo "  To play on this machine too, in another terminal:"
echo "    ./scripts/join.sh local client1 YourName"
echo "=============================================================="
echo

$PRIO ./build/ba_server --secure "$SEC/local_server.xml"
