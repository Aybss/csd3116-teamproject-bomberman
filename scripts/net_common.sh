#!/usr/bin/env bash
# net_common.sh - small helpers used by host.sh and join.sh.
# Do not run this file by itself.

SEC="$ROOT/security/out"
# Where the identity files are read from. host.sh and "join.sh local" use SEC.
# A player who joined with a join code uses SEC/join, so those files never
# overwrite this machine's own host files.
KEYDIR="${KEYDIR:-$SEC}"

# Print this machine's network addresses (not the loopback ones)
show_ips() {
    ip -4 -o addr show scope global 2>/dev/null | awk '$2 != "lo" {print $4}' | cut -d/ -f1
}

# Find a Cyclone security plugin. Example: find_plugin auth
find_plugin() {
    local p
    # -maxdepth 3 reaches /usr/lib/x86_64-linux-gnu/libddsc0/ (Ubuntu) without
    # scanning all of /usr/lib, which takes seconds on WSL.
    p="$(find ${CDDS_LIB_DIR:+"$CDDS_LIB_DIR"} /usr/lib /usr/local/lib /opt/homebrew/lib \
         -maxdepth 3 \( -name "libdds_security_$1.so" -o -name "libdds_security_$1.dylib" \) \
         2>/dev/null | head -n1 || true)"
    echo "${p:-dds_security_$1}"
}

# In WSL, draw with the GPU (D3D12) when it works, not with the CPU (llvmpipe).
# Needs glxinfo (sudo apt install mesa-utils). Does nothing if it cannot check.
use_gpu() {
    if [ -n "${GALLIUM_DRIVER:-}" ] || [ ! -e /dev/dxg ] || ! command -v glxinfo > /dev/null; then
        return 0
    fi
    if GALLIUM_DRIVER=d3d12 glxinfo -B 2> /dev/null | grep -qi "renderer string: d3d12"; then
        export GALLIUM_DRIVER=d3d12
        # Laptops often have a weak Intel GPU and a strong NVIDIA or AMD one.
        # WSL picks the Intel one by default, so ask for the strong one.
        local gpu
        if [ -z "${MESA_D3D12_DEFAULT_ADAPTER_NAME:-}" ]; then
            for gpu in NVIDIA AMD; do
                if MESA_D3D12_DEFAULT_ADAPTER_NAME=$gpu glxinfo -B 2> /dev/null | grep -qi "renderer string: d3d12 ($gpu"; then
                    export MESA_D3D12_DEFAULT_ADAPTER_NAME=$gpu
                    break
                fi
            done
        fi
        echo "Drawing with the GPU: $(glxinfo -B 2> /dev/null | sed -n 's/.*renderer string: //p')"
    elif glxinfo -B 2> /dev/null | grep -qi llvmpipe; then
        echo "Note: the window is drawn by the CPU (llvmpipe), which can feel slow."
        echo "Update the Windows GPU driver and run 'wsl --update' to use the GPU."
    fi
}

# Stop with a clear message if a security file is missing
need_files() {   # $1 = identity (server, client1, ...)
    local f missing=0
    for f in ca_cert.pem governance.p7s permissions.p7s "$1_cert.pem" "$1_key.pem"; do
        if [ ! -f "$KEYDIR/$f" ]; then echo "Missing: ${KEYDIR#"$ROOT"/}/$f"; missing=1; fi
    done
    if [ "$missing" = 1 ]; then
        echo "Join with the code the host shows:  ./scripts/join.sh HOST_ADDRESS JOIN-CODE YourName"
        echo "(Or unpack a bundle from ./scripts/make_bundle.sh into security/out.)"
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
        <IdentityCA>file:$KEYDIR/ca_cert.pem</IdentityCA>
        <IdentityCertificate>file:$KEYDIR/${id}_cert.pem</IdentityCertificate>
        <PrivateKey>file:$KEYDIR/${id}_key.pem</PrivateKey>
      </Authentication>
      <AccessControl>
        <Library path="$ac" initFunction="init_access_control" finalizeFunction="finalize_access_control"/>
        <PermissionsCA>file:$KEYDIR/ca_cert.pem</PermissionsCA>
        <Governance>file:$KEYDIR/governance.p7s</Governance>
        <Permissions>file:$KEYDIR/permissions.p7s</Permissions>
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
