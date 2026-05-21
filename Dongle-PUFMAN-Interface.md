# Dongle ↔ PUFMAN Interface Specification

This document defines:
1. The binary frame protocol between the Arduino (host) and the FPGA (peripheral).
2. The USB serial command interface between the host PC (PUFMAN server) and the Arduino.
3. The complete enrollment data flow, message formats, and cryptographic binding.

---

## 1. Binary Frame Protocol (Arduino ↔ FPGA)

All Arduino ↔ FPGA traffic uses framed binary packets over `Serial1` at 115200
baud. The main Arduino USB serial port still uses human-readable text commands.

### 1.1 Frame Format

All multi-byte integers are little-endian.

```
+------+------+-----+------+-----+-----+---------+--------+--------+--------+
| SOF1 | SOF2 | VER | TYPE | SEQ | CMD | LEN_LO  | LEN_HI | PAYLOAD| CRC_LO | CRC_HI
+------+------+-----+------+-----+-----+---------+--------+--------+--------+
 0xA5   0x5A    1     1B     1B    1B      2B (LE)          0..96B     2B (LE)
```

| Field | Size | Description |
|-------|------|-------------|
| `SOF1` | 1 byte | Start-of-frame byte 1: `0xA5` |
| `SOF2` | 1 byte | Start-of-frame byte 2: `0x5A` |
| `VER`  | 1 byte | Protocol version: `1` |
| `TYPE` | 1 byte | `1` = request, `2` = response, `3` = error |
| `SEQ`  | 1 byte | Sequence number set by Arduino, echoed by FPGA |
| `CMD`  | 1 byte | Command identifier (see §1.3) |
| `LEN`  | 2 bytes LE | Payload length in bytes (0–96) |
| `PAYLOAD` | 0–96 bytes | Command-specific data |
| `CRC`  | 2 bytes LE | CRC16-Modbus over `VER..PAYLOAD` (SOF and CRC bytes excluded) |

CRC algorithm: polynomial `0xA001`, initial value `0xFFFF`.

### 1.2 Error Frame

When the FPGA detects a protocol error it sends a frame with `TYPE = 3` (error).
The payload is a single byte containing the error code:

| Code | Meaning |
|------|---------|
| `1` | Bad CRC |
| `2` | Payload length exceeds maximum |
| `3` | Unsupported protocol version |
| `4` | Unknown command |

Command-specific errors use codes ≥ `1` in the same error frame format.

### 1.3 Command Table

| ID | Name | Request payload | Response payload |
|----|------|-----------------|------------------|
| `1` | `PING` | Any bytes (echoed back) | Same bytes as request |
| `2` | `GET_INFO` | — | `VER(1) MAXPAY_LO(1) MAXPAY_HI(1) FEAT(1) FW_MAJ(1) FW_MIN(1) reserved(2)` |
| `3` | `GET_TIME` | — | 32-bit LE timestamp from `readtime()` |
| `4` | `GET_CA_KEY` | — | 32-byte Ed25519 CA public key |
| `5` | `PUF_GET_FREE_STATE` | — | 1-byte state index (0–10); error if all slots in use |
| `6` | `PUF_RECONFIGURE_STATE` | `state_idx(1) seed_LE(4)` | — |
| `7` | `PUF_CHALLENGE_LR` | `state_idx(1) challenge_id_LE(4)` | 32-byte LR-PUF output (private key seed) |
| `8` | `PUF_SET_DOMAIN` | `state_idx(1) domain_utf8(1..32)` | — |
| `9` | `PUF_STORE_ENROLLMENT` | `state_idx(1) pubkey(32) challenge_raw(16)` | — |
| `10` | `PUF_SAVE_STATES` | — | — |

**Notes:**
- Commands `5`–`10` form the enrollment sequence; they must be called in order.
- `PUF_CHALLENGE_LR` returns the raw LR-PUF output. The Arduino uses this
  directly as the Ed25519 private key seed. The key is never transmitted back to
  the FPGA.
- `PUF_SAVE_STATES` is a blocking SD card write; the Arduino waits up to 3 s
  for the response.

---

## 2. USB Serial Interface (PC ↔ Arduino)

Commands are sent as ASCII lines terminated with `\n` at 115200 baud.

### 2.1 Production Commands

#### `enroll <cert_b64> <payload_b64>`

Performs a full enrollment round-trip:

1. Decodes `cert_b64` (base64) → 128-byte certificate.
2. Fetches the CA public key from the FPGA (CMD 4) if not already cached.
3. Verifies the CA signature on the certificate.
4. Decodes `payload_b64` (base64) → 101-byte signed enrollment payload.
5. Verifies the server signature on the payload using `PK_Server` from the cert.
6. Displays the domain name on the touch-screen; waits for the user to confirm.
7. Prompts for username and password on the touch-screen keyboard.
8. Runs the full key generation and encryption sequence (see §3).
9. Prints `DEVICE_PK`, `DEVICE_SIG`, `LOGIN_TOKEN`, and `ENROLL_COMPLETE`.

Expected output tokens (one per line):

| Token | Meaning |
|-------|---------|
| `CERT_OK` | CA signature on cert is valid |
| `CERT_BAD` | CA signature invalid — enrollment aborted |
| `Domain: <str>` | Domain extracted from cert |
| `PK_Server: <hex>` | Server Ed25519 public key extracted from cert |
| `PAYLOAD_OK` | Server signature on payload is valid |
| `PAYLOAD_BAD` | Server signature invalid — enrollment aborted |
| `ENROLL_REJECTED` | User rejected domain on touch-screen |
| `ENROLL_CANCELLED` | User cancelled credential entry |
| `DEVICE_PK: <64 hex chars>` | Device Ed25519 public key |
| `DEVICE_SIG: <128 hex chars>` | Ed25519 signature over the full 101-byte payload |
| `LOGIN_TOKEN: <hex>` | ECIES-encrypted credential bundle (see §3.4) |
| `ENROLL_COMPLETE` | All steps succeeded |
| `ENROLL_ERROR: <msg>` | Internal failure; message describes cause |

#### `ready`

Returns the single line `READY`. Use this to poll for dongle availability before
sending a long-running command from an automated script.

### 2.2 Debug Commands

| Command | Description |
|---------|-------------|
| `fp_ping` | Protocol PING to FPGA |
| `fp_info` | FPGA version and capability info |
| `fp_time` | FPGA readtime() counter |
| `led_on` / `led_off` | Built-in LED |
| `debug_on` / `debug_off` | Verbose debug output toggle |
| `find_valid` | Scan valid CHOICE-PUF challenges |
| `reconfigure <state_index>` | Reconfigure LR-PUF state (0–10) |
| `keygen <challenge> <state_index>` | PUF-derived Ed25519 keygen |
| `challenge <c> <si> <count> <delay>` | LR-PUF challenge |

---

## 3. Enrollment Data Flow

### 3.1 Certificate Format (128 bytes)

```
Offset  Size  Field
     0    32  Domain  — ASCII, zero-padded to exactly 32 bytes
    32    32  PK_Server — Ed25519 public key of the enrolling server
    64    64  Sig_CA = Ed25519_Sign(SK_CA,  Domain || PK_Server)
```

The certificate is base64-encoded before being passed to the `enroll` command.
Maximum domain length is 31 bytes (one byte reserved for NUL terminator on the
FPGA side; the FPGA stores domains of up to 64 bytes in `domain_name[64]`).

### 3.2 Payload Format (101 bytes)

```
Offset  Size  Field
     0     5  Prefix = ASCII "login"
     5    16  PUF_challenge — 16 random bytes;
              bytes [5..8] (LE uint32) used as challenge_id for the LR-PUF
    21    16  Nonce — 16 random bytes (session nonce, not the encryption nonce)
    37    64  Sig_Server = Ed25519_Sign(SK_Server,  "login" || PUF_challenge || Nonce)
```

The payload is base64-encoded before being passed to the `enroll` command.

### 3.3 Device Key Generation

After the user confirms the domain and enters credentials:

```
1.  FPGA CMD 5  → free_state_idx
2.  seed = generateRandomSeed()   (analog noise from pin A0)
    FPGA CMD 6  (state_idx, seed)  → reconfigure LR-PUF state
3.  challenge_id = payload[5..8]  (little-endian uint32)
    FPGA CMD 7  (state_idx, challenge_id)  → device_privkey[32]
4.  Ed25519::derivePublicKey(device_pubkey, device_privkey)
5.  FPGA CMD 8  (state_idx, domain)
    FPGA CMD 9  (state_idx, device_pubkey, payload[5..20])
    FPGA CMD 10 ()  → save to SD card
6.  device_sig = Ed25519::sign(device_privkey, device_pubkey, payload[0..100])
```

The private key exists only in SRAM during steps 3–6 and is zeroed immediately
after use. It is never stored to flash, SD card, or any non-volatile medium.

### 3.4 LOGIN_TOKEN Construction

The token allows the server to recover the credentials without any shared
secret. It uses ECIES (Elliptic Curve Integrated Encryption Scheme):

```
plaintext = username + "|" + password + "|" + hex(device_pubkey)

Step 1 — convert server's Ed25519 public key to X25519:
    y = PK_Server with sign bit cleared (LE bytes 0..31, bit 255 = 0)
    u = (1 + y) / (1 - y)  mod  (2^255 - 19)   ← birational map
    server_x25519_pk = LE bytes of u

Step 2 — ephemeral X25519 key pair:
    eph_priv = random 32 bytes  (clamped by Curve25519::dh1)
    eph_pub  = X25519(eph_priv, basepoint)

Step 3 — ECDH:
    shared = X25519(eph_priv, server_x25519_pk)

Step 4 — KDF:
    key = SHA-256(shared || eph_pub)     (32 bytes)

Step 5 — ChaCha20-Poly1305 (IETF, 12-byte nonce):
    enc_nonce  = random 12 bytes
    ciphertext, tag = ChaCha20Poly1305(key, enc_nonce).encrypt(plaintext)

Token = eph_pub(32) || enc_nonce(12) || ciphertext(N) || tag(16)
```

**Token decryption** (server side, Python):

```python
import hashlib
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

def decrypt_login_token(token_hex, server_ed25519_seed):
    token     = bytes.fromhex(token_hex)
    eph_pub   = token[0:32]
    nonce     = token[32:44]
    ct_tag    = token[44:]

    # Ed25519 seed -> X25519 scalar (RFC 8032 §5.1.5 / Monocypher)
    h = hashlib.sha512(server_ed25519_seed).digest()
    scalar = bytearray(h[:32])
    scalar[0]  &= 248
    scalar[31] &= 127
    scalar[31] |= 64
    x25519_sk = X25519PrivateKey.from_private_bytes(bytes(scalar))

    shared = x25519_sk.exchange(X25519PublicKey.from_public_bytes(eph_pub))
    key    = hashlib.sha256(shared + eph_pub).digest()
    plain  = ChaCha20Poly1305(key).decrypt(nonce, ct_tag, None)

    username, password, device_pubkey_hex = plain.decode().split("|")
    return username, password, device_pubkey_hex
```

### 3.5 DEVICE_SIG Verification

The device signs the entire 101-byte received payload (including the server's
64-byte signature at bytes 37–100). This proves that the dongle was physically
present and received that exact server-signed session token.

```python
from cryptography.hazmat.primitives.asymmetric import ed25519

def verify_device_sig(payload_bytes, sig_hex, pubkey_hex):
    pk  = ed25519.Ed25519PublicKey.from_public_bytes(bytes.fromhex(pubkey_hex))
    sig = bytes.fromhex(sig_hex)
    pk.verify(sig, payload_bytes)   # raises InvalidSignature if bad
```

`device_pubkey_hex` should be obtained from the decrypted LOGIN_TOKEN and
cross-checked against the `DEVICE_PK` line printed by the dongle.

---

## 4. Security Properties

| Property | Mechanism |
|----------|-----------|
| Server identity | CA-signed Ed25519 certificate; CA key burned into FPGA SD card |
| Session freshness | Server-signed payload with random 16-byte nonce per session |
| Credential confidentiality | ECIES encryption; only the server's private key can decrypt |
| Device binding | Ed25519 device key derived from PUF; private key never stored |
| Session binding | Device signs the full 101-byte payload, tying the device key to this session |
| Replay resistance | `challenge_id` from the server payload selects a different LR-PUF challenge each time |
| Forward secrecy | Ephemeral X25519 key pair per enrollment; `shared` is zeroed after use |
