#!/usr/bin/env bash
# gen_security.sh - create everything DDS Security needs, in security/out/
#
#   ./security/gen_security.sh [NUM_CLIENTS]     (default 5)
#
# Creates:
#   ca_cert.pem / ca_key.pem        our own Certificate Authority (CA)
#   server_cert.pem / _key.pem      identity of the server
#   clientN_cert.pem / _key.pem     identity of each client
#   governance.p7s                  domain rules: encrypt everything, no
#                                   unknown participants (signed by the CA)
#   permissions.p7s                 who may publish/subscribe which topic
#                                   (signed by the CA)
#   server.xml, clientN.xml         Cyclone DDS configs to pass to --secure
#
# The private keys must NEVER be committed (see .gitignore).
# In a real deployment each machine would only get its own key.
set -euo pipefail

NUM_CLIENTS="${1:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
DAYS=365
mkdir -p "$OUT"
cd "$OUT"

# Domain id (0 = default) and topic names (see src/common/config.h)
DOMAIN=0
T_JOIN_REQ="BA_JoinRequest"
T_JOIN_RESP="BA_JoinResponse"
T_INPUT="BA_Input"
T_STATE="BA_GameState"
T_MAP="BA_MapState"

echo "== 1. Certificate Authority"
# EC P-256 keys: supported by Cyclone and faster to handshake than RSA-2048.
openssl ecparam -name prime256v1 -genkey -noout -out ca_key.pem
openssl req -x509 -new -key ca_key.pem -sha256 -days "$DAYS" \
        -subj "/C=SG/O=BlastArena/CN=BlastArena CA" -out ca_cert.pem

make_identity() {   # $1 = name (server, client1, ...)
    openssl ecparam -name prime256v1 -genkey -noout -out "$1_key.pem"
    openssl req -new -key "$1_key.pem" -subj "/C=SG/O=BlastArena/CN=$1" -out "$1.csr"
    openssl x509 -req -in "$1.csr" -CA ca_cert.pem -CAkey ca_key.pem \
            -CAcreateserial -sha256 -days "$DAYS" -out "$1_cert.pem" 2>/dev/null
    rm -f "$1.csr"
}

echo "== 2. Identities"
make_identity server
for i in $(seq 1 "$NUM_CLIENTS"); do make_identity "client$i"; done

echo "== 3. Governance (what protection the domain requires)"
cat > governance.xml <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:noNamespaceSchemaLocation="http://www.omg.org/spec/DDS-SECURITY/20170901/omg_shared_ca_governance.xsd">
  <domain_access_rules>
    <domain_rule>
      <domains><id>$DOMAIN</id></domains>
      <!-- Participants without a CA-signed certificate are ignored -->
      <allow_unauthenticated_participants>false</allow_unauthenticated_participants>
      <!-- Check permissions.p7s when a participant joins -->
      <enable_join_access_control>true</enable_join_access_control>
      <!-- Encrypt discovery, liveliness and every RTPS message -->
      <discovery_protection_kind>ENCRYPT</discovery_protection_kind>
      <liveliness_protection_kind>ENCRYPT</liveliness_protection_kind>
      <rtps_protection_kind>ENCRYPT</rtps_protection_kind>
      <topic_access_rules>
        <topic_rule>
          <topic_expression>BA_*</topic_expression>
          <enable_discovery_protection>true</enable_discovery_protection>
          <enable_liveliness_protection>true</enable_liveliness_protection>
          <enable_read_access_control>true</enable_read_access_control>
          <enable_write_access_control>true</enable_write_access_control>
          <metadata_protection_kind>ENCRYPT</metadata_protection_kind>
          <data_protection_kind>ENCRYPT</data_protection_kind>
        </topic_rule>
      </topic_access_rules>
    </domain_rule>
  </domain_access_rules>
</dds>
EOF

echo "== 4. Permissions (who may publish / subscribe what)"
NOT_BEFORE="$(date -u +%Y-%m-%dT%H:%M:%S)"
NOT_AFTER="$(date -u -d "+$DAYS days" +%Y-%m-%dT%H:%M:%S 2>/dev/null || date -u -v+${DAYS}d +%Y-%m-%dT%H:%M:%S)"

grant() {   # $1 = identity name, $2 = publish topics, $3 = subscribe topics
    local pub="" sub="" t
    for t in $2; do pub="$pub<topic>$t</topic>"; done
    for t in $3; do sub="$sub<topic>$t</topic>"; done
    cat <<EOF
    <grant name="$1_grant">
      <subject_name>CN=$1,O=BlastArena,C=SG</subject_name>
      <validity><not_before>$NOT_BEFORE</not_before><not_after>$NOT_AFTER</not_after></validity>
      <allow_rule>
        <domains><id>$DOMAIN</id></domains>
        <publish><topics>$pub</topics></publish>
        <subscribe><topics>$sub</topics></subscribe>
      </allow_rule>
      <default>DENY</default>
    </grant>
EOF
}

{
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo '<dds xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="http://www.omg.org/spec/DDS-SECURITY/20170901/omg_shared_ca_permissions.xsd">'
    echo '  <permissions>'
    # Only the server may send the game state and join replies.
    grant server "$T_JOIN_RESP $T_STATE $T_MAP" "$T_JOIN_REQ $T_INPUT"
    # Clients may only ask to join and send keys. They cannot read other
    # clients' inputs or fake a GameState.
    for i in $(seq 1 "$NUM_CLIENTS"); do
        grant "client$i" "$T_JOIN_REQ $T_INPUT" "$T_JOIN_RESP $T_STATE $T_MAP"
    done
    echo '  </permissions>'
    echo '</dds>'
} > permissions.xml

echo "== 5. Sign governance and permissions with the CA (S/MIME)"
for f in governance permissions; do
    openssl smime -sign -in "$f.xml" -text -out "$f.p7s" \
            -signer ca_cert.pem -inkey ca_key.pem
done

echo "== 6. Cyclone DDS config files"
# The Debian/Ubuntu package keeps the security plugins in a private folder,
# so give Cyclone the full path when we can find it.
plugin() {
    local p
    p="$(find /usr/lib /usr/local/lib /opt/homebrew/lib 2>/dev/null \
         -name "libdds_security_$1.so" -o -name "libdds_security_$1.dylib" 2>/dev/null | head -n1 || true)"
    echo "${p:-dds_security_$1}"
}
AUTH_LIB="$(plugin auth)"
AC_LIB="$(plugin ac)"
CRYPTO_LIB="$(plugin crypto)"
echo "   auth plugin:   $AUTH_LIB"
if [ "$AUTH_LIB" = "dds_security_auth" ]; then
    echo "   WARNING: libdds_security_auth.so not found. Your Cyclone DDS may be"
    echo "   built without security; see README 'Troubleshooting'."
fi

make_config() {   # $1 = identity name
    cat > "$1.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!-- Generated by security/gen_security.sh - use with --secure $1.xml -->
<CycloneDDS xmlns="https://cdds.io/config">
  <Domain id="any">
    <Security>
      <Authentication>
        <Library path="$AUTH_LIB" initFunction="init_authentication" finalizeFunction="finalize_authentication"/>
        <IdentityCA>file:$OUT/ca_cert.pem</IdentityCA>
        <IdentityCertificate>file:$OUT/$1_cert.pem</IdentityCertificate>
        <PrivateKey>file:$OUT/$1_key.pem</PrivateKey>
      </Authentication>
      <AccessControl>
        <Library path="$AC_LIB" initFunction="init_access_control" finalizeFunction="finalize_access_control"/>
        <PermissionsCA>file:$OUT/ca_cert.pem</PermissionsCA>
        <Governance>file:$OUT/governance.p7s</Governance>
        <Permissions>file:$OUT/permissions.p7s</Permissions>
      </AccessControl>
      <Cryptographic>
        <Library path="$CRYPTO_LIB" initFunction="init_crypto" finalizeFunction="finalize_crypto"/>
      </Cryptographic>
    </Security>
  </Domain>
</CycloneDDS>
EOF
}
make_config server
for i in $(seq 1 "$NUM_CLIENTS"); do make_config "client$i"; done

chmod 600 ./*_key.pem
echo
echo "Done. Files are in $OUT"
echo "  ./build/ba_server --secure security/out/server.xml"
echo "  ./build/ba_client --secure security/out/client1.xml --name Alice"
