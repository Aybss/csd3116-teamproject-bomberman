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

# Prints why the security files must be made again, or nothing if they are fine.
keys_outdated() {
    local ca="$SEC/ca_cert.pem" srv="$SEC/server_cert.pem"
    if [ ! -f "$ca" ] || [ ! -f "$SEC/ca_key.pem" ] || [ ! -f "$srv" ]; then
        echo "No security files yet"
    elif ! openssl x509 -in "$ca" -noout -text | grep -q "Certificate Sign"; then
        echo "The security files are from an older version of this script"
    elif ! openssl x509 -in "$ca" -noout -checkend $((400 * 86400)) > /dev/null; then
        # 400 days > the 365 days of a player certificate, so none outlives the CA
        echo "The CA expires soon"
    elif ! openssl x509 -in "$srv" -noout -checkend $((30 * 86400)) > /dev/null; then
        echo "The server certificate expires soon"
    fi
}
reason="$(keys_outdated)"
if [ -n "$reason" ]; then
    echo "$reason, making new ones..."
    bash ./security/gen_security.sh 5
    if [ "$reason" != "No security files yet" ]; then
        echo "(Players who joined before need the new join code once.)"
    fi
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
