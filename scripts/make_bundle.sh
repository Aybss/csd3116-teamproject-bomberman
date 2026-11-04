#!/usr/bin/env bash
# make_bundle.sh - pack the files one player needs, to send to another machine.
#
#   ./scripts/make_bundle.sh 2      makes security/out/bundle_client2.tar.gz
#
# The bundle has only that player's key, not the CA key or anyone else's key.
set -euo pipefail
cd "$(dirname "$0")/../security/out" 2>/dev/null || { echo "Run ./scripts/host.sh once first."; exit 1; }

N="${1:?Usage: make_bundle.sh NUMBER   (1 to 5)}"
ID="client$N"
[ -f "${ID}_key.pem" ] || { echo "No files for $ID. Run ./scripts/host.sh first."; exit 1; }

tar czf "bundle_$ID.tar.gz" ca_cert.pem governance.p7s permissions.p7s "${ID}_cert.pem" "${ID}_key.pem"
echo "Made: $(pwd)/bundle_$ID.tar.gz"
echo
echo "Send it to the player. In their repo folder they run:"
echo "  mkdir -p security/out && tar xzf bundle_$ID.tar.gz -C security/out"
echo "  ./scripts/join.sh SERVER_ADDRESS $ID TheirName"
