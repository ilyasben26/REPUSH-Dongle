# Enrollment Test Scripts

This directory contains two test scripts and the key material used to test the
full enrollment flow.

---

## Files

| File | Purpose |
|------|---------|
| `test_server_enroll.py` | Full round-trip test: sends enrollment, decrypts LOGIN_TOKEN, verifies DEVICE_SIG |
| `test_enroll.py` | Basic test: sends cert/payload, checks CERT_OK / PAYLOAD_OK / PAYLOAD_BAD |
| `verify.py` | Standalone Ed25519 signature verifier |
| `ca_private.hex` | CA private key seed (32 bytes, 64 hex chars) |
| `ca_public.hex` | CA public key (32 bytes, 64 hex chars) |
| `server_private.hex` | Server private key seed |
| `server_public.hex` | Server public key |

---

## Dependencies

```bash
pip install pyserial cryptography
```

---

## Generating New Keys

If you need fresh key material:

```python
from cryptography.hazmat.primitives.asymmetric import ed25519
import binascii

# CA key pair
ca_sk = ed25519.Ed25519PrivateKey.generate()
open('ca_private.hex', 'w').write(binascii.hexlify(ca_sk.private_bytes_raw()).decode())
open('ca_public.hex',  'w').write(binascii.hexlify(ca_sk.public_key().public_bytes_raw()).decode())

# Server key pair
sv_sk = ed25519.Ed25519PrivateKey.generate()
open('server_private.hex', 'w').write(binascii.hexlify(sv_sk.private_bytes_raw()).decode())
open('server_public.hex',  'w').write(binascii.hexlify(sv_sk.public_key().public_bytes_raw()).decode())
```

After generating new keys you must also update the CA public key compiled into
the FPGA firmware (`CA_PUBKEY` constant in `c_code/main.c`) and re-flash.

---

## Full Enrollment Test: `test_server_enroll.py`

This is the primary test script. It acts as the server: constructs a
CA-signed certificate and a signed payload, sends the `enroll` command to the
Arduino over serial, then verifies everything the dongle returns.

### Usage

```bash
python3 test_server_enroll.py \
    --ca-private-key   ca_private.hex \
    --server-private-key server_private.hex \
    --server-pubkey    server_public.hex \
    --port /dev/cu.usbmodem1201 \
    --domain "example.com"
```

Optional arguments:

| Argument | Default | Description |
|----------|---------|-------------|
| `--domain` | `example.com` | Domain to enroll |
| `--timeout` | `120` | Seconds to wait for ENROLL_COMPLETE (includes touch-screen interaction) |
| `--challenge-hex` | random | Fixed 32 hex-char challenge for reproducibility |
| `--nonce-hex` | random | Fixed 32 hex-char nonce for reproducibility |

### What It Tests

1. **Certificate construction** — builds `Domain(32) || PK_Server(32)` and
   signs it with the CA private key.
2. **Payload construction** — builds `"login" || challenge(16) || nonce(16)`
   and signs it with the server private key.
3. **Serial send** — encodes both as base64 and sends `enroll <cert> <payload>`
   to the Arduino.
4. **Terminal token detection** — reads lines until one of `ENROLL_COMPLETE`,
   `ENROLL_CANCELLED`, `ENROLL_REJECTED`, `CERT_BAD`, `PAYLOAD_BAD`, or
   `ENROLL_ERROR` appears (or timeout).
5. **LOGIN_TOKEN decryption**:
   - Converts the server Ed25519 seed to an X25519 private key via SHA-512
     clamping.
   - Performs X25519 ECDH with the ephemeral public key from the token.
   - Derives `key = SHA-256(shared || eph_pub)`.
   - Decrypts with ChaCha20-Poly1305 (12-byte IETF nonce).
   - Splits plaintext on `|` to recover `username`, `password`,
     `device_pubkey_hex`.
6. **Pubkey cross-check** — verifies that the `device_pubkey_hex` inside the
   decrypted token matches the `DEVICE_PK` line printed by the dongle.
7. **DEVICE_SIG verification** — verifies the Ed25519 signature over the full
   101-byte enrollment payload using the device public key from step 5.

### Expected Output (successful run)

```
[*] Loading keys...
[*] Domain:    example.com
[*] Challenge: <32 hex chars>
[*] Nonce:     <32 hex chars>

[*] Connecting to Arduino.  Interact with the touch screen when prompted.
[*] Waiting up to 120 seconds for ENROLL_COMPLETE...

[>] enroll <cert_b64>... <payload_b64>...
[<] **** Received command: 'enroll ...' ****
[<] CERT_OK
[<] Domain: example.com
[<] PK_Server: <64 hex>
[<] PAYLOAD_OK
... (touch-screen interaction) ...
[<] DEVICE_PK: <64 hex>
[<] DEVICE_SIG: <128 hex>
[<] LOGIN_TOKEN: <hex>
[<] ENROLL_COMPLETE

[*] DEVICE_PK  (32 bytes): <64 hex>
[*] DEVICE_SIG (64 bytes): <first 32 hex>...
[*] LOGIN_TOKEN (N bytes): <first 32 hex>...

[*] Decrypting LOGIN_TOKEN...
[+] username:          <entered username>
[+] password:          ****  (N chars)
[+] device pubkey hex: <64 hex>

[+] DEVICE_PK matches the public key inside the decrypted token.

[*] Verifying DEVICE_SIG over the 101-byte payload...
[+] Signature is VALID — device key authenticated against this session's payload.

============================================================
  ENROLLMENT TEST PASSED
============================================================
  Domain  : example.com
  Username: <username>
  Device PK: <64 hex>
============================================================
```

### Failure Modes

| Output | Cause |
|--------|-------|
| `[FAIL] Certificate rejected` | Arduino printed `CERT_BAD` — CA key mismatch or tampered cert |
| `[FAIL] Payload rejected` | Arduino printed `PAYLOAD_BAD` — server signature invalid |
| `[FAIL] Decryption failed` | Wrong server private key, or LOGIN_TOKEN corrupted |
| `[FAIL] Device pubkey mismatch` | Token and `DEVICE_PK` line disagree — likely a parsing bug |
| `[FAIL] Signature is INVALID` | Device signed with a different key than the one in the token |
| `[!] Timeout` | Arduino did not respond within `--timeout` seconds |

---

## Basic Certificate/Payload Test: `test_enroll.py`

Tests cert and payload verification only (no key generation or LOGIN_TOKEN).
Useful for quickly checking that the CA key on the FPGA matches and that the
server's signing key is correct.

```bash
# Valid certificate and payload
python3 test_enroll.py \
    --ca-private-key ca_private.hex \
    --port /dev/tty.usbmodem11201 \
    enroll \
    --domain "ilyas.com" \
    --server-pubkey server_public.hex \
    --server-private-key server_private.hex

# Tampered certificate (should print CERT_BAD)
python3 test_enroll.py ... enroll-bad ...

# Valid cert but tampered payload signature (should print PAYLOAD_BAD)
python3 test_enroll.py ... enroll-payload-bad ...

# Fixed challenge and nonce for reproducibility
python3 test_enroll.py ... enroll ... \
    --challenge-hex 00112233445566778899aabbccddeeff \
    --nonce-hex     ffeeddccbbaa99887766554433221100
```

---

## LOGIN_TOKEN Format Reference

```
eph_pub(32) || enc_nonce(12) || ciphertext(N) || poly1305_tag(16)
```

- `N` = `len("username|password|" + 64-char pubkey_hex)` (typically 80–140 bytes)
- Total token bytes: `32 + 12 + N + 16 = 60 + N`

The token is printed as lowercase hex on the `LOGIN_TOKEN:` line.
