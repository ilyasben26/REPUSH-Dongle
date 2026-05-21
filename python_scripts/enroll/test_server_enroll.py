#!/usr/bin/env python3
"""
Server-side enrollment test for the REPUSH Dongle.

Performs a complete enrollment round-trip:
  1. Generates a CA-signed server certificate and signed login payload
  2. Sends them to the Arduino via serial as an `enroll` command
  3. Reads back the Arduino's output (requires user to interact with the
     touch screen to confirm the domain and enter credentials)
  4. Decrypts the LOGIN_TOKEN to recover username, password, and the
     device's generated Ed25519 public key
  5. Verifies the DEVICE_SIG (the device signed the full received payload
     with its newly generated private key)

Dependencies:
    pip install pyserial cryptography

Usage:
    python3 test_server_enroll.py \\
        --ca-private-key  ca_private.hex \\
        --server-private-key server_private.hex \\
        --server-pubkey   server_public.hex \\
        --port /dev/cu.usbmodem1201 \\
        --domain "example.com"

Key format: 32-byte seed stored as a 64-character hex file (same as the
other scripts in this directory).
"""

import argparse
import base64
import binascii
import hashlib
import secrets
import sys
import time

try:
    import serial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False
    print("[!] pyserial not installed. Install with: pip install pyserial", file=sys.stderr)

try:
    from cryptography.hazmat.primitives.asymmetric import ed25519
    from cryptography.hazmat.primitives.asymmetric.x25519 import (
        X25519PrivateKey,
        X25519PublicKey,
    )
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
    from cryptography.exceptions import InvalidSignature
    CRYPTO_AVAILABLE = True
except ImportError:
    CRYPTO_AVAILABLE = False
    print("[!] cryptography not installed. Install with: pip install cryptography", file=sys.stderr)

# ── Constants matching Arduino/FPGA definitions ──────────────────────────────

CERT_DOMAIN_BYTES    = 32
CERT_PK_SERVER_BYTES = 32
CERT_SIG_BYTES       = 64
CERT_TOTAL_BYTES     = CERT_DOMAIN_BYTES + CERT_PK_SERVER_BYTES + CERT_SIG_BYTES  # 128

PAYLOAD_PREFIX         = b"login"
PAYLOAD_CHALLENGE_BYTES = 16
PAYLOAD_NONCE_BYTES     = 16
PAYLOAD_MESSAGE_BYTES  = len(PAYLOAD_PREFIX) + PAYLOAD_CHALLENGE_BYTES + PAYLOAD_NONCE_BYTES  # 37
PAYLOAD_TOTAL_BYTES    = PAYLOAD_MESSAGE_BYTES + CERT_SIG_BYTES  # 101

# LOGIN_TOKEN layout:  eph_pub(32) || nonce(12) || ciphertext(N) || tag(16)
TOKEN_EPH_PUB_BYTES = 32
TOKEN_NONCE_BYTES   = 12
TOKEN_TAG_BYTES     = 16
TOKEN_HEADER_BYTES  = TOKEN_EPH_PUB_BYTES + TOKEN_NONCE_BYTES  # 44

# ── Key helpers ───────────────────────────────────────────────────────────────

def load_hex_key(path: str) -> bytes:
    with open(path) as f:
        return binascii.unhexlify(f.read().strip())


def ed25519_seed_to_x25519_sk(ed_seed: bytes) -> X25519PrivateKey:
    """
    Derive the X25519 private key that corresponds to an Ed25519 seed.

    Algorithm (matches Monocypher crypto_from_ed25519_private):
      scalar = SHA-512(seed)[0:32]
      scalar[0]  &= 248   (clear bits 0-2)
      scalar[31] &= 127   (clear bit 7)
      scalar[31] |= 64    (set  bit 6)
    """
    h = hashlib.sha512(ed_seed).digest()
    scalar = bytearray(h[:32])
    scalar[0]  &= 248
    scalar[31] &= 127
    scalar[31] |= 64
    return X25519PrivateKey.from_private_bytes(bytes(scalar))


# ── Cert / payload construction ───────────────────────────────────────────────

def create_certificate(domain: str, server_pubkey: bytes, ca_seed: bytes) -> bytes:
    domain_padded = domain.encode() + b"\x00" * (CERT_DOMAIN_BYTES - len(domain.encode()))
    message = domain_padded + server_pubkey
    ca_sk = ed25519.Ed25519PrivateKey.from_private_bytes(ca_seed[:32])
    signature = ca_sk.sign(message)
    return domain_padded + server_pubkey + signature


def create_signed_payload(server_seed: bytes,
                          challenge: bytes | None = None,
                          nonce: bytes | None = None):
    if challenge is None:
        challenge = secrets.token_bytes(PAYLOAD_CHALLENGE_BYTES)
    if nonce is None:
        nonce = secrets.token_bytes(PAYLOAD_NONCE_BYTES)
    message = PAYLOAD_PREFIX + challenge + nonce
    sk = ed25519.Ed25519PrivateKey.from_private_bytes(server_seed[:32])
    signature = sk.sign(message)
    return message + signature, challenge, nonce


# ── Decryption ────────────────────────────────────────────────────────────────

def decrypt_login_token(token_hex: str, server_ed_seed: bytes) -> dict:
    """
    Decrypt the LOGIN_TOKEN printed by the Arduino and return a dict with
    keys 'username', 'password', 'device_pubkey_hex'.

    Token layout: eph_pub(32) || nonce(12) || ciphertext(N) || tag(16)

    Decryption steps:
      1. x25519_sk  = ed25519_seed_to_x25519_sk(server_ed_seed)
      2. shared     = X25519(x25519_sk, eph_pub)
      3. key        = SHA256(shared || eph_pub)
      4. plaintext  = ChaCha20Poly1305(key).decrypt(nonce, ciphertext||tag)
      5. split on '|' -> username, password, device_pubkey_hex
    """
    token = bytes.fromhex(token_hex)
    if len(token) < TOKEN_HEADER_BYTES + TOKEN_TAG_BYTES + 1:
        raise ValueError(f"Token too short: {len(token)} bytes")

    eph_pub_bytes     = token[:TOKEN_EPH_PUB_BYTES]
    nonce             = token[TOKEN_EPH_PUB_BYTES : TOKEN_HEADER_BYTES]
    ciphertext_and_tag = token[TOKEN_HEADER_BYTES:]

    x25519_sk = ed25519_seed_to_x25519_sk(server_ed_seed)
    eph_pub_obj = X25519PublicKey.from_public_bytes(eph_pub_bytes)
    shared = x25519_sk.exchange(eph_pub_obj)

    key = hashlib.sha256(shared + eph_pub_bytes).digest()

    chacha = ChaCha20Poly1305(key)
    plaintext = chacha.decrypt(nonce, ciphertext_and_tag, None)

    parts = plaintext.decode("utf-8").split("|")
    if len(parts) != 3:
        raise ValueError(f"Unexpected plaintext format: {plaintext!r}")

    return {
        "username":         parts[0],
        "password":         parts[1],
        "device_pubkey_hex": parts[2],
    }


# ── Signature verification ────────────────────────────────────────────────────

def verify_device_signature(payload: bytes,
                             device_sig_hex: str,
                             device_pubkey_hex: str) -> bool:
    """
    Verify that the device signed the full 101-byte payload with its
    generated Ed25519 private key.
    """
    device_sig    = bytes.fromhex(device_sig_hex)
    device_pubkey = bytes.fromhex(device_pubkey_hex)
    pk_obj = ed25519.Ed25519PublicKey.from_public_bytes(device_pubkey)
    try:
        pk_obj.verify(device_sig, payload)
        return True
    except InvalidSignature:
        return False


# ── Serial communication ──────────────────────────────────────────────────────

def send_and_collect(port: str, command: str, timeout_s: float = 120.0) -> list[str]:
    """
    Send one line to the Arduino and collect all output until ENROLL_COMPLETE,
    ENROLL_CANCELLED, ENROLL_REJECTED, CERT_BAD, PAYLOAD_BAD, or timeout.
    Lines are printed to stdout as they arrive.
    """
    terminal_tokens = {
        "ENROLL_COMPLETE", "ENROLL_CANCELLED", "ENROLL_REJECTED",
        "CERT_BAD", "PAYLOAD_BAD", "ENROLL_ERROR",
    }

    lines = []
    with serial.Serial(port, 115200, timeout=1.0) as ser:
        time.sleep(1.5)           # let the CDC connection settle
        ser.reset_input_buffer()
        ser.write((command + "\n").encode())
        ser.flush()
        print(f"[>] {command[:80]}{'...' if len(command) > 80 else ''}")

        deadline = time.time() + timeout_s
        buf = ""
        while time.time() < deadline:
            raw = ser.read(ser.in_waiting or 1).decode("utf-8", errors="replace")
            if not raw:
                continue
            buf += raw
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.rstrip("\r")
                if line:
                    print(f"[<] {line}")
                    lines.append(line)
                    if any(tok in line for tok in terminal_tokens):
                        return lines

    print("[!] Timeout waiting for terminal response.", file=sys.stderr)
    return lines


def parse_response(lines: list[str]) -> dict:
    result = {}
    for line in lines:
        for key in ("DEVICE_PK", "DEVICE_SIG", "LOGIN_TOKEN"):
            prefix = f"{key}: "
            if line.startswith(prefix):
                result[key] = line[len(prefix):].strip()
    return result


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Server-side enrollment test for REPUSH Dongle",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--ca-private-key",    required=True, help="CA private key seed (hex file)")
    parser.add_argument("--server-private-key", required=True, help="Server private key seed (hex file)")
    parser.add_argument("--server-pubkey",      required=True, help="Server public key (hex file)")
    parser.add_argument("--port",              required=True, help="Serial port (e.g. /dev/cu.usbmodem1201)")
    parser.add_argument("--domain",            default="example.com", help="Domain name for enrollment")
    parser.add_argument("--timeout",           type=float, default=120.0,
                        help="Seconds to wait for Arduino response (default 120; includes touch-screen interaction)")
    parser.add_argument("--challenge-hex", help="Optional fixed 16-byte challenge as 32 hex chars")
    parser.add_argument("--nonce-hex",     help="Optional fixed 16-byte nonce as 32 hex chars")
    args = parser.parse_args()

    if not SERIAL_AVAILABLE or not CRYPTO_AVAILABLE:
        sys.exit(1)

    # ── Load keys ────────────────────────────────────────────────────────────
    print(f"[*] Loading keys...")
    ca_seed     = load_hex_key(args.ca_private_key)
    server_seed = load_hex_key(args.server_private_key)
    server_pub  = load_hex_key(args.server_pubkey)

    # ── Build cert & payload ─────────────────────────────────────────────────
    cert = create_certificate(args.domain, server_pub, ca_seed)
    assert len(cert) == CERT_TOTAL_BYTES

    challenge = binascii.unhexlify(args.challenge_hex) if args.challenge_hex else None
    nonce     = binascii.unhexlify(args.nonce_hex)     if args.nonce_hex     else None
    payload, challenge, nonce = create_signed_payload(server_seed, challenge, nonce)
    assert len(payload) == PAYLOAD_TOTAL_BYTES

    cert_b64    = base64.b64encode(cert).decode()
    payload_b64 = base64.b64encode(payload).decode()

    print(f"[*] Domain:    {args.domain}")
    print(f"[*] Challenge: {challenge.hex()}")
    print(f"[*] Nonce:     {nonce.hex()}")
    print()
    print("[*] Connecting to Arduino.  Interact with the touch screen when prompted.")
    print(f"[*] Waiting up to {args.timeout:.0f} seconds for ENROLL_COMPLETE...")
    print()

    # ── Send to Arduino ──────────────────────────────────────────────────────
    command = f"enroll {cert_b64} {payload_b64}"
    lines   = send_and_collect(args.port, command, timeout_s=args.timeout)
    parsed  = parse_response(lines)

    print()

    # ── Check for protocol-level failures ───────────────────────────────────
    full_output = "\n".join(lines)
    if "CERT_BAD" in full_output:
        print("[FAIL] Certificate rejected by Arduino.", file=sys.stderr)
        sys.exit(1)
    if "PAYLOAD_BAD" in full_output:
        print("[FAIL] Payload rejected by Arduino.", file=sys.stderr)
        sys.exit(1)
    if "ENROLL_REJECTED" in full_output:
        print("[FAIL] User rejected the enrollment on the touch screen.", file=sys.stderr)
        sys.exit(1)
    if "ENROLL_CANCELLED" in full_output:
        print("[FAIL] User cancelled credential entry on the touch screen.", file=sys.stderr)
        sys.exit(1)
    if "ENROLL_ERROR" in full_output:
        print("[FAIL] Arduino reported an enrollment error.", file=sys.stderr)
        sys.exit(1)
    if "ENROLL_COMPLETE" not in full_output:
        print("[FAIL] ENROLL_COMPLETE not received.", file=sys.stderr)
        sys.exit(1)

    # ── Validate we got all three fields ────────────────────────────────────
    missing = [k for k in ("DEVICE_PK", "DEVICE_SIG", "LOGIN_TOKEN") if k not in parsed]
    if missing:
        print(f"[FAIL] Missing fields in response: {missing}", file=sys.stderr)
        sys.exit(1)

    device_pk_hex  = parsed["DEVICE_PK"]
    device_sig_hex = parsed["DEVICE_SIG"]
    token_hex      = parsed["LOGIN_TOKEN"]

    print(f"[*] DEVICE_PK  ({len(device_pk_hex)//2} bytes): {device_pk_hex}")
    print(f"[*] DEVICE_SIG ({len(device_sig_hex)//2} bytes): {device_sig_hex[:32]}...")
    print(f"[*] LOGIN_TOKEN ({len(token_hex)//2} bytes): {token_hex[:32]}...")
    print()

    # ── Step 1: decrypt the login token ──────────────────────────────────────
    print("[*] Decrypting LOGIN_TOKEN...")
    try:
        decrypted = decrypt_login_token(token_hex, server_seed)
    except Exception as e:
        print(f"[FAIL] Decryption failed: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[+] username:          {decrypted['username']}")
    print(f"[+] password:          {'*' * len(decrypted['password'])}  ({len(decrypted['password'])} chars)")
    print(f"[+] device pubkey hex: {decrypted['device_pubkey_hex']}")
    print()

    # ── Step 2: cross-check the device pubkey from token vs. DEVICE_PK line ─
    pk_from_token = decrypted["device_pubkey_hex"].upper()
    pk_from_line  = device_pk_hex.upper()
    if pk_from_token != pk_from_line:
        print(f"[FAIL] Device pubkey mismatch!", file=sys.stderr)
        print(f"       from DEVICE_PK line : {pk_from_line}",  file=sys.stderr)
        print(f"       from LOGIN_TOKEN    : {pk_from_token}", file=sys.stderr)
        sys.exit(1)
    print("[+] DEVICE_PK matches the public key inside the decrypted token.")
    print()

    # ── Step 3: verify the device signature over the full payload ────────────
    print("[*] Verifying DEVICE_SIG over the 101-byte payload...")
    if verify_device_signature(payload, device_sig_hex, decrypted["device_pubkey_hex"]):
        print("[+] Signature is VALID — device key authenticated against this session's payload.")
    else:
        print("[FAIL] Signature is INVALID.", file=sys.stderr)
        sys.exit(1)

    print()
    print("=" * 60)
    print("  ENROLLMENT TEST PASSED")
    print("=" * 60)
    print(f"  Domain  : {args.domain}")
    print(f"  Username: {decrypted['username']}")
    print(f"  Device PK: {decrypted['device_pubkey_hex']}")
    print("=" * 60)


if __name__ == "__main__":
    main()
