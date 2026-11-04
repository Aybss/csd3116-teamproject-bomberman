#!/usr/bin/env bash
# host.sh - start the game server so other machines can join.
#
#   ./scripts/host.sh                      secure server
#   ./scripts/host.sh 10.0.0.5 10.0.0.6    also contact these player addresses
#   INSECURE=1 ./scripts/host.sh           no security (for quick tests only)
#   DDS_TRACE=1 ./scripts/host.sh          write a debug log to /tmp/dds.log
#
# The first time it makes the security files for you.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/scripts/net_common.sh"

[ -x build/ba_server ] || { echo "Build first: cmake -B build && cmake --build build"; exit 1; }

echo "Addresses of this machine (give one of them to the players):"
show_ips | sed 's/^/  /'
echo

if [ "${INSECURE:-}" = "1" ]; then
    echo "INSECURE mode: no authentication, no encryption."
    echo "Players join with:  INSECURE=1 ./scripts/join.sh SERVER_ADDRESS"
    write_config none "$SEC/local_none.xml" "$@"
    exec ./build/ba_server --secure "$SEC/local_none.xml"
fi

if [ ! -f "$SEC/server_cert.pem" ]; then
    echo "No security files yet, making them..."
    bash ./security/gen_security.sh 5
    echo
fi
need_files server
write_config server "$SEC/local_server.xml" "$@"
echo "Players on other machines need their own bundle. Make one with:"
echo "  ./scripts/make_bundle.sh 1      (then 2, 3, ...)"
echo "They join with:  ./scripts/join.sh SERVER_ADDRESS client1 TheirName"
echo
exec ./build/ba_server --secure "$SEC/local_server.xml"
