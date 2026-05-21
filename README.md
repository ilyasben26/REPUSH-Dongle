# REPUSH Dongle

A hardware-rooted password manager dongle for a master thesis project. The
dongle stores credentials encrypted inside a physically unique device — the
secret key is never stored anywhere; it is reconstructed from silicon on demand
using a Physical Unclonable Function (PUF).

---

## What It Does

A user enrolls with a web service by plugging in the dongle and touching a few
on-screen prompts. The dongle:

1. Verifies the server's identity with a CA-signed certificate (Ed25519).
2. Asks the user to confirm the domain and enter a username/password on the
   built-in touch-screen keyboard.
3. Derives a device-unique Ed25519 key pair from the LR-PUF response to the
   server's challenge — the private key is never stored anywhere.
4. Signs the received server payload with the device private key so the server
   can later prove the dongle was physically present.
5. Encrypts the username, password, and device public key into a LOGIN_TOKEN
   that only the server can decrypt (ECIES/ChaCha20-Poly1305).
6. Persists the enrollment record (domain, public key, challenge parameters) to
   the FPGA's SD card for future authentication sessions.

---

## Hardware

| Component | Role |
|-----------|------|
| Arduino Nano 33 IoT (SAMD21) | Host controller: USB serial interface, touch-screen, crypto operations, FPGA RPC client |
| Tang Nano 20K (GW2AR-18) | PUF peripheral, LR-PUF state machine, SD card storage, CA key storage |
| ILI9341 2.8" TFT + STMPE610 touch | User interface — domain confirmation + credential entry |

The Arduino and FPGA communicate over a binary-framed UART protocol on
`Serial1` (pins TX1/RX1, 115200 baud).

---

## Repository Layout

```
REPUSH-Dongle/
├── src/
│   ├── main.cpp            Arduino firmware (enrollment, crypto, FPGA RPC)
│   ├── puf_functions.cpp   Arduino-side (legacy) PUF helpers + RNG
│   └── touch_keyboard.cpp  ILI9341 touch-screen UI
├── include/
│   ├── puf_functions.h
│   └── touch_keyboard.h
├── python_scripts/
│   └── enroll/
│       ├── test_enroll.py          Basic cert/payload test (no key generation)
│       ├── test_server_enroll.py   Full enrollment round-trip test (server side)
│       ├── verify.py               Standalone signature verifier
│       └── TEST_ENROLL_README.md   How to run the tests
├── Dongle-PUFMAN-Interface.md  Full protocol specification
└── README.md                   This file

picorv32_tang_nano_unified/     (separate repository, FPGA side)
├── c_code/
│   ├── main.c      FPGA firmware + binary protocol dispatcher
│   ├── puf.c / puf.h  PUF driver, LR-PUF state machine, SD persistence
│   └── monocypher.c   Monocypher 4.0.2 (Ed25519, Blake2b)
├── src/
│   ├── puf_peripheral.v    Memory-mapped CHOICE-PUF wrapper
│   └── top.v               SoC top level
├── PUF_INTEGRATION.md      Hardware and software PUF integration notes
└── CA-keys.md              How the CA and server keys were generated
```

---

## Building and Flashing

### Arduino

```bash
# Install PlatformIO, then:
pio run -e nano_33_iot -t upload --upload-port /dev/cu.usbmodem11201
```

Dependencies (auto-fetched by PlatformIO):
- `Arduino-Ed25519` — Ed25519 sign/verify/keygen
- `Crypto` (rweather) — `Curve25519`, `ChaChaPoly`, `SHA256`
- `Adafruit ILI9341`, `Adafruit STMPE610`, `Adafruit GFX`

### FPGA firmware

```bash
cd picorv32_tang_nano_unified/c_code
make clean && make 20k
# Then synthesize in Gowin IDE and program Tang Nano 20K
```

Requires: `riscv64-unknown-elf-gcc` (Ubuntu: `sudo apt install gcc-riscv64-unknown-elf`).

---

## Key Concepts

### Physical Unclonable Function (PUF)

A PUF exploits manufacturing variations in silicon to produce a device-unique
response to a given challenge. The same challenge always produces the same
response on a specific chip, but the response cannot be predicted or reproduced
on a different chip.

This project uses a **CHOICE-PUF** implemented in FPGA fabric: 64 parallel
ASR-based carry-chain race circuits, each producing one response bit. The result
is a 64-bit measurement.

### LR-PUF (Lookup-and-Reconstruct PUF)

The raw CHOICE-PUF output is 64 bits — not enough for a 256-bit key. The LR-PUF
extends this by chaining multiple PUF challenges through a Blake2b hash:

1. A set of valid challenges (configurations that produce stable, non-degenerate
   responses) is scanned and saved to the SD card.
2. A 32-bit `challenge_id` selects a starting challenge from this table.
3. The firmware runs a sequence of PUF measurements, feeding each 64-bit
   response through `Blake2b-256` along with the previous hash, accumulating
   entropy across multiple challenges until 256 bits of output are produced.

The final 32-byte value is used directly as an Ed25519 private key seed. The
private key is reconstructed each time from the PUF — it is never stored on
any persistent medium.

### puf_state_t — Per-Enrollment Record

Each enrollment occupies one **LR-PUF state slot** on the FPGA (up to 11 slots,
stored on SD card blocks 4-7). A slot records:

| Field | Size | Purpose |
|-------|------|---------|
| `hash_value` | 32 bytes | Blake2b running hash (LR-PUF state) |
| `is_initialized` | 4 bytes | Non-zero when slot is in use |
| `tc_last`, `tt_last`, `bc_last`, `bt_last` | 4 × 4 bytes | Last PUF challenge parameters |
| `domain_name` | 64 bytes | UTF-8 domain string, NUL-terminated |
| `pubkey` | 32 bytes | Device Ed25519 public key |
| `challenge_raw` | 16 bytes | The 16-byte challenge from the enrollment payload |
| `acknowledged` | 4 bytes | 0 = pending server confirmation, 1 = confirmed |

---

## The Enrollment Protocol

```
Server                                    Dongle
  |                                          |
  |-- enroll <cert_b64> <payload_b64> -----> |
  |                                          | 1. Verify CA signature on cert
  |                                          | 2. Extract domain, PK_Server from cert
  |                                          | 3. Verify server signature on payload
  |                                          | 4. Display domain on touch-screen
  |                                          | 5. User confirms domain
  |                                          | 6. User enters username + password
  |                                          | 7. Get free LR-PUF state slot (FPGA)
  |                                          | 8. Reconfigure state with fresh random seed
  |                                          | 9. challenge_id = payload bytes [5..8] (LE)
  |                                          |10. Run LR-PUF → 32-byte private key seed
  |                                          |11. Ed25519::derivePublicKey(seed) → pubkey
  |                                          |12. Store domain + pubkey + challenge on FPGA SD
  |                                          |13. Sign full 101-byte payload → DEVICE_SIG
  |                                          |14. Ed25519-to-X25519 conversion (local GF arith)
  |                                          |15. ECIES encrypt(username|password|pubkey_hex)
  |                                          |      → LOGIN_TOKEN
  | <-- DEVICE_PK: <hex> ------------------- |
  | <-- DEVICE_SIG: <hex> ------------------ |
  | <-- LOGIN_TOKEN: <hex> ----------------- |
  | <-- ENROLL_COMPLETE -------------------- |
```

### Certificate Format (128 bytes, base64-encoded for transport)

```
Offset  Size  Field
     0    32  Domain name (ASCII, zero-padded)
    32    32  PK_Server (Ed25519 public key)
    64    64  Signature = Ed25519_Sign(SK_CA, Domain || PK_Server)
```

### Payload Format (101 bytes, base64-encoded for transport)

```
Offset  Size  Field
     0     5  ASCII "login"
     5    16  PUF_challenge (random 16 bytes; bytes [5..8] used as challenge_id)
    21    16  Nonce (random 16 bytes)
    37    64  Signature = Ed25519_Sign(SK_Server, "login" || PUF_challenge || Nonce)
```

### LOGIN_TOKEN (hex-encoded in serial output)

```
Offset  Size  Field
     0    32  eph_pub  — ephemeral X25519 public key
    32    12  nonce    — 12-byte IETF ChaCha20-Poly1305 nonce
    44     N  ciphertext — ChaCha20-Poly1305 encryption of plaintext
  44+N    16  tag      — Poly1305 authentication tag
```

**Plaintext**: `username|password|DEVICE_PK_HEX` (pipe-separated UTF-8)

**Decryption** (Python example in `test_server_enroll.py`):
1. `x25519_sk = SHA512(server_ed25519_seed)[0:32]` clamped (bits 0-2 and 255 cleared, bit 254 set)
2. `shared = X25519(x25519_sk, eph_pub)`
3. `key = SHA256(shared || eph_pub)`
4. `plaintext = ChaCha20Poly1305(key).decrypt(nonce, ciphertext || tag, aad=None)`
5. Split `plaintext` on `|` → `username`, `password`, `device_pubkey_hex`

---

## Cryptographic Primitives

| Operation | Algorithm | Where |
|-----------|-----------|-------|
| Server cert signing | Ed25519 | Server (Python `cryptography`) |
| Cert verification | Ed25519 | Arduino (Arduino-Ed25519 lib) |
| Payload signing | Ed25519 | Server |
| Payload verification | Ed25519 | Arduino |
| Device key generation | Ed25519 (seed from PUF) | Arduino (keygen) + FPGA (PUF) |
| Device payload signing | Ed25519 | Arduino |
| PK conversion for encryption | GF(2^255-19) birational map | Arduino (local, TweetNaCl-style) |
| ECDH key agreement | X25519 | Arduino (Crypto lib) |
| KDF | SHA-256 | Arduino (Crypto lib) |
| Symmetric encryption | ChaCha20-Poly1305 | Arduino (Crypto lib) |
| LR-PUF state hashing | Blake2b-256 | FPGA (Monocypher 4.0.2) |

### Ed25519 → X25519 Public Key Conversion

The server's signing key is Ed25519 but ECIES requires an X25519 key for ECDH.
Since both curves are birationally equivalent (both are instances of
Bernstein's Curve25519), the conversion is deterministic and lossless:

```
u = (1 + y) / (1 - y)  mod  p      where p = 2^255 - 19
```

`y` is the y-coordinate extracted from the 32-byte Ed25519 public key
(little-endian, sign bit cleared). The result `u` is the X25519 public key.

This is implemented on the Arduino in `src/main.cpp` using TweetNaCl-style
GF(2^255-19) field arithmetic (16-limb representation, `int64_t` per limb).
The conversion was moved from the FPGA to the Arduino because the FPGA's
PicoRV32 has a limited stack and `crypto_eddsa_to_x25519` from Monocypher
caused a stack overflow when called from inside the protocol dispatch chain.

---

## Arduino Serial Commands

Commands are sent over USB serial at 115200 baud.

### Production commands

| Command | Description |
|---------|-------------|
| `enroll <cert_b64> <payload_b64>` | Full enrollment flow |
| `ready` | Returns `READY` — use to poll before sending a command |

### Debug / development commands

| Command | Description |
|---------|-------------|
| `fp_ping` | Ping the FPGA over the binary protocol |
| `fp_info` | Query FPGA protocol version, max payload, FW version |
| `fp_time` | Read FPGA `readtime()` counter |
| `led_on` / `led_off` | Toggle the built-in LED |
| `debug_on` / `debug_off` | Toggle verbose debug output |
| `find_valid` | Scan and print valid CHOICE-PUF challenges |
| `reconfigure <state_index>` | Reconfigure LR-PUF state |
| `keygen <challenge> <state_index>` | Derive and print an Ed25519 public key |
| `challenge <c> <si> <count> <delay>` | Run LR-PUF challenge, print result |

### Enrollment output tokens

| Token | Meaning |
|-------|---------|
| `CERT_OK` | CA signature on cert verified |
| `CERT_BAD` | CA signature invalid |
| `PAYLOAD_OK` | Server signature on payload verified |
| `PAYLOAD_BAD` | Server signature invalid |
| `ENROLL_REJECTED` | User rejected domain on touch-screen |
| `ENROLL_CANCELLED` | User cancelled credential entry |
| `ENROLL_COMPLETE` | Enrollment succeeded; DEVICE_PK/DEVICE_SIG/LOGIN_TOKEN printed |
| `ENROLL_ERROR: ...` | Internal error (message follows colon) |

---

## Testing Enrollment

See `python_scripts/enroll/TEST_ENROLL_README.md` for full instructions.

Quick start (keys already generated in `python_scripts/enroll/`):

```bash
pip install pyserial cryptography

python3 python_scripts/enroll/test_server_enroll.py \
    --ca-private-key  python_scripts/enroll/ca_private.hex \
    --server-private-key python_scripts/enroll/server_private.hex \
    --server-pubkey   python_scripts/enroll/server_public.hex \
    --port /dev/cu.usbmodem1201 \
    --domain "example.com"
```

The script will send the enrollment command, wait for the user to interact with
the touch screen, then decrypt the LOGIN_TOKEN and verify the DEVICE_SIG.

---

## Binary Protocol (Arduino ↔ FPGA)

See `Dongle-PUFMAN-Interface.md` for the complete frame format and all command
definitions.

Frame layout:
```
SOF1(0xA5) SOF2(0x5A) VER TYPE SEQ CMD LEN_LO LEN_HI PAYLOAD... CRC_LO CRC_HI
```
CRC16-Modbus (poly `0xA001`, init `0xFFFF`) over `VER..PAYLOAD`.
