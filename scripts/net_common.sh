#!/usr/bin/env bash
# net_common.sh - small helpers used by host.sh and join.sh.
# Do not run this file by itself.

SEC="$ROOT/security/out"

# Print this machine's network addresses (not the loopback ones)
show_ips() {
    ip -4 -o addr show scope global 2>/dev/null | awk '$2 != "lo" {print $4}' | cut -d/ -f1
}

# Find a Cyclone security plugin. Example: find_plugin auth
find_plugin() {
    local p
    p="$(find /usr/lib /usr/local/lib /opt/homebrew/lib "${CDDS_LIB_DIR:-/usr/lib}" \
         \( -name "libdds_security_$1.so" -o -name "libdds_security_$1.dylib" \) \
         2>/dev/null | head -n1 || true)"
    echo "${p:-dds_security_$1}"
}

# Stop with a clear message if a security file is missing
need_files() {   # $1 = identity (server, client1, ...)
    local f missing=0
    for f in ca_cert.pem governance.p7s permissions.p7s "$1_cert.pem" "$1_key.pem"; do
        if [ ! -f "$SEC/$f" ]; then echo "Missing: security/out/$f"; missing=1; fi
    done
    if [ "$missing" = 1 ]; then
        echo "The host makes the files with ./security/gen_security.sh and packs them"
        echo "for one player with ./scripts/make_bundle.sh NUMBER. Unpack that bundle with:"
        echo "  mkdir -p security/out && tar xzf bundle_clientN.tar.gz -C security/out"
        exit 1
    fi
}

# write_config IDENTITY OUTFILE [PEER_IP ...]
#   IDENTITY  server, client1, ... or "none" for no security
#   PEER_IP   when given, multicast is turned off and only these addresses
#             are contacted (more reliable on Wi-Fi than multicast)
write_config() {
    local id="$1" out="$2" p
    shift 2
    mkdir -p "$SEC"
    local auth ac crypto
    auth="$(find_plugin auth)"; ac="$(find_plugin ac)"; crypto="$(find_plugin crypto)"
    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<CycloneDDS xmlns="https://cdds.io/config">'
        echo '  <Domain id="any">'
        if [ "${DDS_TRACE:-}" = "1" ]; then
            echo '    <Tracing><Category>config,discovery,warning,error</Category><OutputFile>/tmp/dds.log</OutputFile></Tracing>'
        fi
        if [ "$#" -gt 0 ]; then
            echo '    <General><AllowMulticast>false</AllowMulticast></General>'
        fi
        printf '    <Discovery><ParticipantIndex>auto</ParticipantIndex>'
        if [ "$#" -gt 0 ]; then
            printf '<Peers>'
            for p in "$@"; do printf '<Peer address="%s"/>' "$p"; done
            printf '</Peers>'
        fi
        echo '</Discovery>'
        if [ "$id" != "none" ]; then
            cat <<EOF
    <Security>
      <Authentication>
        <Library path="$auth" initFunction="init_authentication" finalizeFunction="finalize_authentication"/>
        <IdentityCA>file:$SEC/ca_cert.pem</IdentityCA>
        <IdentityCertificate>file:$SEC/${id}_cert.pem</IdentityCertificate>
        <PrivateKey>file:$SEC/${id}_key.pem</PrivateKey>
      </Authentication>
      <AccessControl>
        <Library path="$ac" initFunction="init_access_control" finalizeFunction="finalize_access_control"/>
        <PermissionsCA>file:$SEC/ca_cert.pem</PermissionsCA>
        <Governance>file:$SEC/governance.p7s</Governance>
        <Permissions>file:$SEC/permissions.p7s</Permissions>
      </AccessControl>
      <Cryptographic>
        <Library path="$crypto" initFunction="init_crypto" finalizeFunction="finalize_crypto"/>
      </Cryptographic>
    </Security>
EOF
        fi
        echo '  </Domain>'
        echo '</CycloneDDS>'
    } > "$out"
}
