#!/usr/bin/env python3
"""join_code.py - give players their DDS Security files over the network.

The host runs `serve` next to the game server and prints a short join code.
A player runs `fetch` with the host's address and that code, and gets:

    ca_cert.pem, clientN_cert.pem, clientN_key.pem,
    governance.p7s, permissions.p7s

How it stays safe (like a Wi-Fi password, not like a public website):
  * The player makes its own private key. It never leaves the player's machine;
    only a certificate signing request (public key) is sent.
  * Both messages carry an HMAC keyed with the join code (stretched with
    PBKDF2). The host only signs requests from someone who knows the code, and
    the player only accepts a CA that came from someone who knows the code,
    so a stranger on the network can neither get a certificate nor slip in a
    fake CA.
  * The player's random nonce is in both MACs, so an old reply cannot be
    replayed.
  * The host stops handing out certificates after MAX_FAILURES wrong codes.

Uses only the Python standard library and the openssl command.

    python3 join_code.py serve --dir security/out --code K7QX-M2PA
    python3 join_code.py fetch --host 10.0.0.5 --code K7QX-M2PA --dir security/out/join
    python3 join_code.py new-code
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import secrets
import socket
import subprocess
import sys
import tempfile

PORT = 7390
KDF_SALT = b"BlastArena join code v1"
KDF_ROUNDS = 200_000
MAX_MSG = 64 * 1024
MAX_FAILURES = 20
# No 0/O, 1/I/L: easy to read out loud. 31^8 is about 8.5e11 codes.
ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
SUBJECT = "/C=SG/O=BlastArena/CN={}"   # must match security/gen_security.sh


def log(msg):
    print(f"[join-code] {msg}", flush=True)


def new_code():
    s = "".join(secrets.choice(ALPHABET) for _ in range(8))
    return f"{s[:4]}-{s[4:]}"


def normalize(code):
    return "".join(c for c in code.upper() if c.isalnum())


def code_key(code):
    return hashlib.pbkdf2_hmac("sha256", normalize(code).encode(), KDF_SALT, KDF_ROUNDS)


def mac(key, *parts):
    """HMAC over length-prefixed parts, so parts cannot be shifted around."""
    h = hmac.new(key, digestmod=hashlib.sha256)
    for p in parts:
        b = p.encode() if isinstance(p, str) else p
        h.update(len(b).to_bytes(4, "big"))
        h.update(b)
    return h.hexdigest()


def send(sock, obj):
    sock.sendall(json.dumps(obj).encode() + b"\n")


def recv(sock):
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        if len(data) > MAX_MSG:
            raise ValueError("message too large")
    return json.loads(data.decode())


def openssl(*args, data=None):
    r = subprocess.run(["openssl", *args], input=data, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.decode(errors="replace").strip())
    return r.stdout


def read(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def write(path, content, private=False):
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    fd = os.open(path, flags, 0o600 if private else 0o644)
    with os.fdopen(fd, "wb" if isinstance(content, bytes) else "w") as f:
        f.write(content)


# ---------------------------------------------------------------- host side

def serve(args):
    d = args.dir
    for f in ("ca_cert.pem", "ca_key.pem", "governance.p7s", "permissions.p7s"):
        if not os.path.isfile(os.path.join(d, f)):
            sys.exit(f"[join-code] Missing {d}/{f}. Run security/gen_security.sh first.")
    key = code_key(args.code)
    ca = read(os.path.join(d, "ca_cert.pem"))
    gov = base64.b64encode(read(os.path.join(d, "governance.p7s"), "rb")).decode()
    perm = base64.b64encode(read(os.path.join(d, "permissions.p7s"), "rb")).decode()
    slots = [f"client{i}" for i in range(args.first, args.last + 1)]
    given = 0
    failures = 0

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", args.port))
    except OSError as e:
        sys.exit(f"[join-code] Cannot use TCP port {args.port}: {e}. "
                 "Is another host.sh already running?")
    srv.listen(5)
    log(f"Waiting for players on TCP port {args.port}")

    while failures < MAX_FAILURES:
        conn, (ip, _) = srv.accept()
        with conn:
            conn.settimeout(10)
            try:
                req = recv(conn)
                nonce, csr = str(req["nonce"]), str(req["csr"])
                if not hmac.compare_digest(str(req["mac"]), mac(key, "req", nonce, csr)):
                    failures += 1
                    log(f"{ip}: wrong join code ({failures}/{MAX_FAILURES})")
                    send(conn, {"error": "wrong join code"})
                    continue
                slot = slots[given % len(slots)]
                with tempfile.TemporaryDirectory() as tmp:
                    csr_path = os.path.join(tmp, "req.csr")
                    cert_path = os.path.join(tmp, "cert.pem")
                    write(csr_path, csr)
                    # Verifies the request's own signature, then signs it with
                    # the name we choose (not the name the player asked for).
                    openssl("x509", "-req", "-in", csr_path,
                            "-CA", os.path.join(d, "ca_cert.pem"),
                            "-CAkey", os.path.join(d, "ca_key.pem"),
                            "-CAcreateserial", "-sha256", "-days", "365",
                            "-subj", SUBJECT.format(slot), "-out", cert_path)
                    cert = read(cert_path)
                send(conn, {"slot": slot, "ca": ca, "cert": cert,
                            "governance": gov, "permissions": perm,
                            "mac": mac(key, "resp", nonce, slot, ca, cert, gov, perm)})
                given += 1
                log(f"{ip}: gave certificate {slot}")
            except (OSError, ValueError, KeyError, RuntimeError) as e:
                log(f"{ip}: bad request ({e})")
    log("Too many wrong codes, no more certificates this session. "
        "Restart host.sh for a new code.")


# -------------------------------------------------------------- player side

def fetch(args):
    d = args.dir
    os.makedirs(d, exist_ok=True)
    key = code_key(args.code)
    nonce = secrets.token_hex(16)

    with tempfile.TemporaryDirectory() as tmp:
        key_path = os.path.join(tmp, "key.pem")
        openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", key_path)
        csr = openssl("req", "-new", "-key", key_path,
                      "-subj", SUBJECT.format("player")).decode()

        try:
            with socket.create_connection((args.host, args.port), timeout=10) as s:
                s.settimeout(15)
                send(s, {"nonce": nonce, "csr": csr, "mac": mac(key, "req", nonce, csr)})
                resp = recv(s)
        except (OSError, ValueError) as e:
            sys.exit(f"[join-code] Cannot get the files from {args.host}:{args.port} ({e}).\n"
                     "  Is ./scripts/host.sh running there? On Windows, run\n"
                     "  scripts\\windows_setup.ps1 again on the host (it opens TCP 7390).")

        if "error" in resp:
            sys.exit(f"[join-code] Host said: {resp['error']}. Check the code.")
        try:
            slot, ca, cert = resp["slot"], resp["ca"], resp["cert"]
            gov, perm = resp["governance"], resp["permissions"]
            ok = hmac.compare_digest(resp["mac"],
                                     mac(key, "resp", nonce, slot, ca, cert, gov, perm))
        except (KeyError, TypeError):
            ok = False
        if not ok or slot not in {f"client{i}" for i in range(1, 6)}:
            sys.exit("[join-code] The reply was not from a host with this code. Not using it.")

        # Remove files from an earlier host, then save the new ones.
        for f in os.listdir(d):
            if f.endswith((".pem", ".p7s")) or f == "identity.txt":
                os.remove(os.path.join(d, f))
        write(os.path.join(d, "ca_cert.pem"), ca)
        write(os.path.join(d, f"{slot}_cert.pem"), cert)
        write(os.path.join(d, f"{slot}_key.pem"), read(key_path), private=True)
    write(os.path.join(d, "governance.p7s"), base64.b64decode(gov))
    write(os.path.join(d, "permissions.p7s"), base64.b64decode(perm))
    write(os.path.join(d, "identity.txt"), slot + "\n")

    openssl("verify", "-CAfile", os.path.join(d, "ca_cert.pem"),
            os.path.join(d, f"{slot}_cert.pem"))
    log(f"Got certificate {slot} from {args.host}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="host: hand out certificates")
    s.add_argument("--dir", required=True)
    s.add_argument("--code", required=True)
    s.add_argument("--port", type=int, default=PORT)
    s.add_argument("--first", type=int, default=2, help="first client number to give out")
    s.add_argument("--last", type=int, default=5, help="last client number to give out")
    f = sub.add_parser("fetch", help="player: get certificates from a host")
    f.add_argument("--host", required=True)
    f.add_argument("--code", required=True)
    f.add_argument("--dir", required=True)
    f.add_argument("--port", type=int, default=PORT)
    sub.add_parser("new-code", help="print a random join code")
    args = ap.parse_args()

    if args.cmd == "serve":
        serve(args)
    elif args.cmd == "fetch":
        fetch(args)
    else:
        print(new_code())


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
