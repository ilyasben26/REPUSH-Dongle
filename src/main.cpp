#include <Arduino.h>
#include <Ed25519.h>
#include <Curve25519.h>
#include <ChaChaPoly.h>
#include <SHA256.h>
#include <string.h>
#include "puf_functions.h"
#include "touch_keyboard.h"

int led = LED_BUILTIN;

bool debug_mode = false;

static constexpr uint8_t PROTO_SOF1 = 0xA5;
static constexpr uint8_t PROTO_SOF2 = 0x5A;
static constexpr uint8_t PROTO_VERSION = 1;
static constexpr uint8_t PROTO_MSG_REQ = 1;
static constexpr uint8_t PROTO_MSG_RSP = 2;
static constexpr uint8_t PROTO_MSG_ERR = 3;
static constexpr uint8_t PROTO_CMD_PING = 1;
static constexpr uint8_t PROTO_CMD_GET_INFO = 2;
static constexpr uint8_t PROTO_CMD_GET_TIME = 3;
static constexpr uint8_t PROTO_CMD_GET_CA_KEY = 4;
static constexpr uint8_t PROTO_CMD_PUF_GET_FREE_STATE    = 5;
static constexpr uint8_t PROTO_CMD_PUF_RECONFIGURE_STATE = 6;
static constexpr uint8_t PROTO_CMD_PUF_CHALLENGE_LR      = 7;
static constexpr uint8_t PROTO_CMD_PUF_SET_DOMAIN        = 8;
static constexpr uint8_t PROTO_CMD_PUF_STORE_ENROLLMENT  = 9;
static constexpr uint8_t PROTO_CMD_PUF_SAVE_STATES           = 10;
static constexpr uint8_t PROTO_CMD_PUF_FIND_STATE_BY_DOMAIN  = 11;
static constexpr uint8_t PROTO_CMD_PUF_MARK_ACKNOWLEDGED     = 12;
static constexpr uint8_t PROTO_CMD_PUF_CLEAR_STATES          = 13;
static constexpr uint8_t PROTO_CMD_PUF_GET_SLOT_STATUS       = 14;
static constexpr size_t PROTO_MAX_PAYLOAD = 96;
static constexpr size_t CERT_DOMAIN_BYTES = 32;
static constexpr size_t CERT_PK_SERVER_BYTES = 32;
static constexpr size_t CERT_SIG_BYTES = 64;
static constexpr size_t CERT_TOTAL_BYTES = CERT_DOMAIN_BYTES + CERT_PK_SERVER_BYTES + CERT_SIG_BYTES;

static constexpr char ENROLL_PAYLOAD_PREFIX[] = "login";
static constexpr size_t ENROLL_PAYLOAD_PREFIX_BYTES = 5;
static constexpr size_t ENROLL_CHALLENGE_BYTES = 16;
static constexpr size_t ENROLL_NONCE_BYTES = 16;
static constexpr size_t ENROLL_PAYLOAD_MSG_BYTES = ENROLL_PAYLOAD_PREFIX_BYTES + ENROLL_CHALLENGE_BYTES + ENROLL_NONCE_BYTES;
static constexpr size_t ENROLL_PAYLOAD_TOTAL_BYTES = ENROLL_PAYLOAD_MSG_BYTES + CERT_SIG_BYTES;

// Error-correction parameters
static constexpr uint8_t ENROLL_PUF_SAMPLES  = 10; // per-bit majority-vote samples during enrolment
static constexpr uint8_t QUERY_PUF_MAX_TRIES = 20; // max re-query attempts during Phase 2 signing

// Phase 2 sensitive payload layout (all sizes in bytes):
//   "sensitive"(9) + C_Session(16) + N(16) + Description(32) + Server_Sig(64) = 137
static constexpr char   SENSITIVE_PAYLOAD_PREFIX[]      = "sensitive";
static constexpr size_t SENSITIVE_PAYLOAD_PREFIX_BYTES  = 9;
static constexpr size_t SENSITIVE_CHALLENGE_BYTES       = 16;
static constexpr size_t SENSITIVE_NONCE_BYTES           = 16;
static constexpr size_t SENSITIVE_DESCRIPTION_BYTES     = 32;
static constexpr size_t SENSITIVE_PAYLOAD_MSG_BYTES     =
    SENSITIVE_PAYLOAD_PREFIX_BYTES + SENSITIVE_CHALLENGE_BYTES +
    SENSITIVE_NONCE_BYTES + SENSITIVE_DESCRIPTION_BYTES;
static constexpr size_t SENSITIVE_PAYLOAD_TOTAL_BYTES   = SENSITIVE_PAYLOAD_MSG_BYTES + CERT_SIG_BYTES;

static uint8_t ca_pubkey[32] = {0};
static bool ca_pubkey_loaded = false;

struct ProtoFrame
{
  uint8_t version;
  uint8_t msg_type;
  uint8_t seq;
  uint8_t cmd;
  uint16_t len;
  uint8_t payload[PROTO_MAX_PAYLOAD];
};

uint16_t proto_crc16_update(uint16_t crc, uint8_t data)
{
  crc ^= data;
  for (uint8_t i = 0; i < 8; i++)
  {
    if (crc & 1)
      crc = (crc >> 1) ^ 0xA001;
    else
      crc >>= 1;
  }
  return crc;
}

void fpga_flush_rx()
{
  while (Serial1.available())
    Serial1.read();
}

void fpga_send_frame(uint8_t msg_type, uint8_t seq, uint8_t cmd, const uint8_t *payload, uint16_t len)
{
  uint16_t crc = 0xffff;
  uint8_t b = 0;

  Serial1.write(PROTO_SOF1);
  Serial1.write(PROTO_SOF2);

  b = PROTO_VERSION;
  Serial1.write(b);
  crc = proto_crc16_update(crc, b);

  b = msg_type;
  Serial1.write(b);
  crc = proto_crc16_update(crc, b);

  b = seq;
  Serial1.write(b);
  crc = proto_crc16_update(crc, b);

  b = cmd;
  Serial1.write(b);
  crc = proto_crc16_update(crc, b);

  b = static_cast<uint8_t>(len & 0xff);
  Serial1.write(b);
  crc = proto_crc16_update(crc, b);

  b = static_cast<uint8_t>((len >> 8) & 0xff);
  Serial1.write(b);
  crc = proto_crc16_update(crc, b);

  for (uint16_t i = 0; i < len; i++)
  {
    Serial1.write(payload[i]);
    crc = proto_crc16_update(crc, payload[i]);
  }

  Serial1.write(static_cast<uint8_t>(crc & 0xff));
  Serial1.write(static_cast<uint8_t>((crc >> 8) & 0xff));
}

bool fpga_read_frame(ProtoFrame &frame, uint32_t timeout_ms)
{
  enum ParseState
  {
    WAIT_SOF1,
    WAIT_SOF2,
    READ_HEADER,
    READ_PAYLOAD,
    READ_CRC_LO,
    READ_CRC_HI
  };

  ParseState state = WAIT_SOF1;
  uint8_t header[6] = {0};
  uint16_t crc = 0xffff;
  uint16_t header_index = 0;
  uint16_t payload_index = 0;
  uint8_t crc_lo = 0;
  uint32_t start = millis();

  while ((millis() - start) < timeout_ms)
  {
    if (!Serial1.available())
      continue;

    uint8_t ch = static_cast<uint8_t>(Serial1.read());

    switch (state)
    {
    case WAIT_SOF1:
      if (ch == PROTO_SOF1)
        state = WAIT_SOF2;
      break;

    case WAIT_SOF2:
      if (ch == PROTO_SOF2)
      {
        state = READ_HEADER;
        header_index = 0;
        payload_index = 0;
        crc = 0xffff;
      }
      else if (ch != PROTO_SOF1)
      {
        state = WAIT_SOF1;
      }
      break;

    case READ_HEADER:
      header[header_index++] = ch;
      crc = proto_crc16_update(crc, ch);
      if (header_index == sizeof(header))
      {
        frame.version = header[0];
        frame.msg_type = header[1];
        frame.seq = header[2];
        frame.cmd = header[3];
        frame.len = static_cast<uint16_t>(header[4]) | (static_cast<uint16_t>(header[5]) << 8);

        if (frame.version != PROTO_VERSION || frame.len > PROTO_MAX_PAYLOAD)
        {
          state = WAIT_SOF1;
        }
        else if (frame.len == 0)
        {
          state = READ_CRC_LO;
        }
        else
        {
          state = READ_PAYLOAD;
        }
      }
      break;

    case READ_PAYLOAD:
      frame.payload[payload_index++] = ch;
      crc = proto_crc16_update(crc, ch);
      if (payload_index >= frame.len)
        state = READ_CRC_LO;
      break;

    case READ_CRC_LO:
      crc_lo = ch;
      state = READ_CRC_HI;
      break;

    case READ_CRC_HI:
    {
      uint16_t rx_crc = static_cast<uint16_t>(crc_lo) | (static_cast<uint16_t>(ch) << 8);
      if (rx_crc == crc)
        return true;
      state = WAIT_SOF1;
      break;
    }
    }
  }

  return false;
}

static constexpr int     FPGA_RPC_MAX_RETRIES  = 3;
static constexpr uint32_t FPGA_RPC_RETRY_DELAY = 150; // ms between retries

bool fpga_rpc(uint8_t cmd, const uint8_t *request_payload, uint16_t request_len, ProtoFrame &response, uint32_t timeout_ms)
{
  if (request_len > PROTO_MAX_PAYLOAD)
    return false;

  static uint8_t seq = 1;

  for (int attempt = 0; attempt < FPGA_RPC_MAX_RETRIES; attempt++)
  {
    if (attempt > 0)
    {
      delay(FPGA_RPC_RETRY_DELAY);
      Serial.print("[FPGA] timeout, retry ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.print(FPGA_RPC_MAX_RETRIES - 1);
      Serial.print(" cmd=0x");
      Serial.println(cmd, HEX);
    }

    uint8_t  tx_seq = seq++;
    uint32_t start  = millis();

    fpga_flush_rx();
    fpga_send_frame(PROTO_MSG_REQ, tx_seq, cmd, request_payload, request_len);

    while ((millis() - start) < timeout_ms)
    {
      uint32_t remaining = timeout_ms - (millis() - start);
      if (!fpga_read_frame(response, remaining))
        break; // timed out on this attempt — go to next retry

      if (response.seq == tx_seq && response.cmd == cmd)
        return true;
      // wrong seq/cmd: keep waiting (may be a stale frame from a prior op)
    }
  }

  return false;
}

void print_hex_bytes(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++)
  {
    if (data[i] < 16)
      Serial.print("0");
    Serial.print(data[i], HEX);
  }
}

int b64_value(char ch)
{
  if (ch >= 'A' && ch <= 'Z')
    return ch - 'A';
  if (ch >= 'a' && ch <= 'z')
    return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9')
    return ch - '0' + 52;
  if (ch == '+')
    return 62;
  if (ch == '/')
    return 63;
  return -1;
}

bool decode_base64(const String &input, uint8_t *out, size_t out_max, size_t &out_len)
{
  uint32_t accum = 0;
  uint8_t bits = 0;
  bool saw_padding = false;

  out_len = 0;

  for (size_t i = 0; i < input.length(); i++)
  {
    char ch = input.charAt(i);

    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
      continue;

    if (ch == '=')
    {
      saw_padding = true;
      continue;
    }

    int v = b64_value(ch);
    if (v < 0 || saw_padding)
      return false;

    accum = (accum << 6) | static_cast<uint32_t>(v);
    bits = static_cast<uint8_t>(bits + 6);

    if (bits >= 8)
    {
      bits = static_cast<uint8_t>(bits - 8);
      if (out_len >= out_max)
        return false;
      out[out_len++] = static_cast<uint8_t>((accum >> bits) & 0xff);
    }
  }

  if (bits > 0)
  {
    uint32_t mask = (1u << bits) - 1u;
    if ((accum & mask) != 0)
      return false;
  }

  return true;
}

static void fill_random(uint8_t *buf, size_t len)
{
  size_t i = 0;
  while (i < len)
  {
    uint32_t r = generateRandomSeed();
    size_t chunk = (len - i < 4) ? (len - i) : 4;
    for (size_t j = 0; j < chunk; j++)
      buf[i++] = static_cast<uint8_t>((r >> (j * 8)) & 0xFF);
  }
}

bool fpga_puf_find_state_by_domain(const char *domain,
                                    uint8_t &state_index,
                                    uint8_t pubkey[32],
                                    uint8_t challenge_raw[16],
                                    uint8_t pk_server[32])
{
  ProtoFrame response = {};
  size_t dlen = strlen(domain);
  if (dlen > 63) dlen = 63;
  if (!fpga_rpc(PROTO_CMD_PUF_FIND_STATE_BY_DOMAIN,
                reinterpret_cast<const uint8_t *>(domain),
                static_cast<uint16_t>(dlen), response, 2000))
  {
    Serial.println("Error: fpga_puf_find_state_by_domain timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
    return false;
  if (response.len < 81)
  {
    Serial.println("Error: fpga_puf_find_state_by_domain short response.");
    return false;
  }
  state_index = response.payload[0];
  memcpy(pubkey,        response.payload + 1,  32);
  memcpy(challenge_raw, response.payload + 33, 16);
  memcpy(pk_server,     response.payload + 49, 32);
  return true;
}

bool fpga_puf_mark_acknowledged(uint8_t state_index)
{
  ProtoFrame response = {};
  if (!fpga_rpc(PROTO_CMD_PUF_MARK_ACKNOWLEDGED, &state_index, 1, response, 3000))
  {
    Serial.println("Error: fpga_puf_mark_acknowledged timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA failed to mark acknowledged.");
    return false;
  }
  return true;
}

// GF(2^255-19) field arithmetic for Ed25519 public key -> X25519 public key conversion.
// Algorithm: u = (1+y) / (1-y) mod p  (birational map Edwards -> Montgomery)
// Uses TweetNaCl-style 16-limb representation (16 x 16-bit limbs, int64_t for overflow headroom).

typedef int64_t gf25519[16];

static void gf_carry(gf25519 o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)65536;
        int64_t c = o[i] >> 16;
        if (i < 15) o[i + 1] += c - 1;
        else        o[0]     += 38 * (c - 1);
        o[i] -= c << 16;
    }
}

static void gf_mul(gf25519 o, const gf25519 a, const gf25519 b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    gf_carry(o);
    gf_carry(o);
}

static void gf_inv(gf25519 o, const gf25519 a) {
    // Fermat: a^(p-2) mod p, p-2 = 2^255-21, all bits set except bits 2 and 4
    gf25519 c;
    for (int i = 0; i < 16; i++) c[i] = a[i];
    for (int i = 253; i >= 0; i--) {
        gf_mul(c, c, c); // square
        if (i != 2 && i != 4) gf_mul(c, c, a);
    }
    for (int i = 0; i < 16; i++) o[i] = c[i];
}

static void gf_from_bytes(gf25519 o, const uint8_t b[32]) {
    for (int i = 0; i < 16; i++)
        o[i] = (int64_t)b[2*i] | ((int64_t)b[2*i+1] << 8);
}

static void gf_to_bytes(uint8_t b[32], gf25519 n) {
    gf25519 m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    gf_carry(t); gf_carry(t); gf_carry(t);
    // Subtract p twice to get canonical representative in [0, p)
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t borrow = (m[15] >> 16) & 1; // 1 if t < p (keep t), 0 if t >= p (use m)
        m[14] &= 0xffff;
        // Conditionally swap t and m: swap when borrow=0 (t >= p, use reduced m)
        int64_t mask = -(int64_t)(1 - borrow);
        for (int i = 0; i < 16; i++) {
            int64_t d = mask & (t[i] ^ m[i]);
            t[i] ^= d;
            m[i] ^= d;
        }
    }
    for (int i = 0; i < 16; i++) {
        b[2*i]   = static_cast<uint8_t>(t[i] & 0xff);
        b[2*i+1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

// Convert Ed25519 public key to Curve25519 (X25519) public key.
// Implements u = (1+y) / (1-y) mod p from the birational equivalence.
static void ed25519_pk_to_x25519(uint8_t out[32], const uint8_t ed_pk[32]) {
    uint8_t tmp[32];
    memcpy(tmp, ed_pk, 32);
    tmp[31] &= 0x7f; // clear sign bit to get y coordinate
    gf25519 y, num, den;
    gf_from_bytes(y, tmp);
    for (int i = 0; i < 16; i++) { num[i] = y[i]; den[i] = -y[i]; }
    num[0] += 1; // num = 1 + y
    den[0] += 1; // den = 1 - y
    gf_inv(den, den);
    gf_mul(num, num, den); // u = (1+y) / (1-y)
    gf_to_bytes(out, num);
}

bool fetch_ca_key_from_fpga()
{
  ProtoFrame response = {};

  if (!fpga_rpc(PROTO_CMD_GET_CA_KEY, nullptr, 0, response, 1000))
  {
    Serial.println("Error: failed to fetch CA key from FPGA (timeout).");
    return false;
  }

  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA returned error fetching CA key.");
    return false;
  }

  if (response.len != 32)
  {
    Serial.println("Error: CA key response has invalid length.");
    return false;
  }

  for (size_t i = 0; i < 32; i++)
    ca_pubkey[i] = response.payload[i];

  ca_pubkey_loaded = true;
  return true;
}

bool fpga_puf_get_free_state(uint8_t &state_index)
{
  ProtoFrame response = {};
  if (!fpga_rpc(PROTO_CMD_PUF_GET_FREE_STATE, nullptr, 0, response, 2000))
  {
    Serial.println("Error: fpga_puf_get_free_state timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: All PUF state slots are in use.");
    return false;
  }
  if (response.len < 1)
  {
    Serial.println("Error: fpga_puf_get_free_state short response.");
    return false;
  }
  state_index = response.payload[0];
  return true;
}

bool fpga_puf_reconfigure_state(uint8_t state_index, uint32_t seed)
{
  uint8_t req[5];
  ProtoFrame response = {};
  req[0] = state_index;
  req[1] = static_cast<uint8_t>(seed & 0xFF);
  req[2] = static_cast<uint8_t>((seed >> 8) & 0xFF);
  req[3] = static_cast<uint8_t>((seed >> 16) & 0xFF);
  req[4] = static_cast<uint8_t>((seed >> 24) & 0xFF);
  if (!fpga_rpc(PROTO_CMD_PUF_RECONFIGURE_STATE, req, sizeof(req), response, 5000))
  {
    Serial.println("Error: fpga_puf_reconfigure_state timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA rejected PUF state reconfiguration.");
    return false;
  }
  return true;
}

bool fpga_puf_challenge_lr(uint8_t state_index, uint32_t challenge_id, uint8_t out[32], uint8_t count = 1)
{
  uint8_t req[6];
  ProtoFrame response = {};
  req[0] = state_index;
  req[1] = static_cast<uint8_t>(challenge_id & 0xFF);
  req[2] = static_cast<uint8_t>((challenge_id >> 8) & 0xFF);
  req[3] = static_cast<uint8_t>((challenge_id >> 16) & 0xFF);
  req[4] = static_cast<uint8_t>((challenge_id >> 24) & 0xFF);
  req[5] = (count == 0) ? 1 : count;
  uint32_t timeout_ms = 5000u + 500u * static_cast<uint32_t>(req[5]);
  if (!fpga_rpc(PROTO_CMD_PUF_CHALLENGE_LR, req, sizeof(req), response, timeout_ms))
  {
    Serial.println("Error: fpga_puf_challenge_lr timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA rejected LR-PUF challenge.");
    return false;
  }
  if (response.len < 32)
  {
    Serial.println("Error: fpga_puf_challenge_lr short response.");
    return false;
  }
  memcpy(out, response.payload, 32);
  return true;
}

bool fpga_puf_set_domain(uint8_t state_index, const char *domain)
{
  uint8_t req[1 + 32];
  ProtoFrame response = {};
  req[0] = state_index;
  size_t dlen = strlen(domain);
  if (dlen > 32)
    dlen = 32;
  memcpy(req + 1, domain, dlen);
  if (!fpga_rpc(PROTO_CMD_PUF_SET_DOMAIN, req, static_cast<uint16_t>(1 + dlen), response, 2000))
  {
    Serial.println("Error: fpga_puf_set_domain timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA rejected set domain.");
    return false;
  }
  return true;
}

bool fpga_puf_store_enrollment(uint8_t state_index,
                               const uint8_t pubkey[32],
                               const uint8_t challenge_raw[16],
                               const uint8_t pk_server[32])
{
  uint8_t req[81];
  ProtoFrame response = {};
  req[0] = state_index;
  memcpy(req + 1,  pubkey,        32);
  memcpy(req + 33, challenge_raw, 16);
  memcpy(req + 49, pk_server,     32);
  if (!fpga_rpc(PROTO_CMD_PUF_STORE_ENROLLMENT, req, sizeof(req), response, 2000))
  {
    Serial.println("Error: fpga_puf_store_enrollment timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA rejected store enrollment.");
    return false;
  }
  return true;
}

bool fpga_puf_save_states()
{
  ProtoFrame response = {};
  if (!fpga_rpc(PROTO_CMD_PUF_SAVE_STATES, nullptr, 0, response, 3000))
  {
    Serial.println("Error: fpga_puf_save_states timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA failed to save states.");
    return false;
  }
  return true;
}

bool fpga_puf_clear_states()
{
  ProtoFrame response = {};
  if (!fpga_rpc(PROTO_CMD_PUF_CLEAR_STATES, nullptr, 0, response, 5000))
  {
    Serial.println("Error: fpga_puf_clear_states timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA failed to clear states.");
    return false;
  }
  return true;
}

bool fpga_puf_get_slot_status(uint8_t state_index,
                               uint8_t &is_init,
                               uint8_t &acknowledged,
                               char domain_buf[65])
{
  ProtoFrame response = {};
  if (!fpga_rpc(PROTO_CMD_PUF_GET_SLOT_STATUS, &state_index, 1, response, 2000))
  {
    Serial.println("Error: fpga_puf_get_slot_status timeout.");
    return false;
  }
  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.println("Error: FPGA failed to get slot status.");
    return false;
  }
  if (response.len < 2)
  {
    Serial.println("Error: fpga_puf_get_slot_status short response.");
    return false;
  }
  is_init      = response.payload[0];
  acknowledged = response.payload[1];
  size_t dlen  = response.len - 2;
  if (dlen > 64) dlen = 64;
  memcpy(domain_buf, response.payload + 2, dlen);
  domain_buf[dlen] = '\0';
  return true;
}

// Phase 2 error correction: re-query the LR-PUF (count=1 each time) until the
// derived Ed25519 public key matches the pubkey stored on the FPGA during enrolment.
// Returns true and fills out_privkey on success; false after QUERY_PUF_MAX_TRIES.
static bool puf_query_match(uint8_t state_index, uint32_t challenge_id,
                             const uint8_t stored_pubkey[32], uint8_t out_privkey[32])
{
  for (int attempt = 1; attempt <= QUERY_PUF_MAX_TRIES; attempt++)
  {
    uint8_t seed[32] = {0};
    if (!fpga_puf_challenge_lr(state_index, challenge_id, seed))
    {
      Serial.print("[PUF-EC] Attempt ");
      Serial.print(attempt);
      Serial.println(": FPGA error, retrying...");
      continue;
    }

    uint8_t derived_pk[32];
    Ed25519::derivePublicKey(derived_pk, seed);

    if (memcmp(derived_pk, stored_pubkey, 32) == 0)
    {
      Serial.print("[PUF-EC] Key reproduced on attempt ");
      Serial.println(attempt);
      memcpy(out_privkey, seed, 32);
      memset(seed, 0, sizeof(seed));
      return true;
    }

    Serial.print("[PUF-EC] Attempt ");
    Serial.print(attempt);
    Serial.println(": pubkey mismatch, retrying...");
    memset(seed, 0, sizeof(seed));
  }

  Serial.println("[PUF-EC] Error: key not reproduced within max attempts.");
  return false;
}

bool verify_cert_locally(const uint8_t *cert, size_t cert_len)
{
  const uint8_t *message = cert;
  const uint8_t *signature = cert + CERT_DOMAIN_BYTES + CERT_PK_SERVER_BYTES;

  if (cert_len != CERT_TOTAL_BYTES)
    return false;

  if (!ca_pubkey_loaded)
  {
    Serial.println("Error: CA public key not loaded.");
    return false;
  }

  return Ed25519::verify(signature, ca_pubkey, message, CERT_DOMAIN_BYTES + CERT_PK_SERVER_BYTES);
}

void print_domain_from_cert(const uint8_t *cert)
{
  char domain[CERT_DOMAIN_BYTES + 1] = {0};
  size_t domain_len = 0;

  while (domain_len < CERT_DOMAIN_BYTES && cert[domain_len] != 0)
  {
    domain[domain_len] = static_cast<char>(cert[domain_len]);
    domain_len++;
  }

  Serial.print("Domain: ");
  if (domain_len == 0)
    Serial.println("<empty>");
  else
    Serial.println(domain);
}

void print_server_pubkey_from_cert(const uint8_t *cert)
{
  Serial.print("PK_Server: ");
  print_hex_bytes(cert + CERT_DOMAIN_BYTES, CERT_PK_SERVER_BYTES);
  Serial.println();
}

bool verify_enroll_payload(const uint8_t *cert, const uint8_t *payload, size_t payload_len)
{
  const uint8_t *server_pubkey = cert + CERT_DOMAIN_BYTES;
  const uint8_t *message = payload;
  const uint8_t *signature = payload + ENROLL_PAYLOAD_MSG_BYTES;

  if (payload_len != ENROLL_PAYLOAD_TOTAL_BYTES)
  {
    Serial.print("Error: payload must decode to ");
    Serial.print(ENROLL_PAYLOAD_TOTAL_BYTES);
    Serial.println(" bytes.");
    return false;
  }

  if (memcmp(message, ENROLL_PAYLOAD_PREFIX, ENROLL_PAYLOAD_PREFIX_BYTES) != 0)
  {
    Serial.println("Error: payload prefix must be 'login'.");
    return false;
  }

  return Ed25519::verify(signature, server_pubkey, message, ENROLL_PAYLOAD_MSG_BYTES);
}

void do_fpga_ping()
{
  ProtoFrame response = {};
  const uint8_t ping_payload[] = {'P', 'I', 'N', 'G'};

  if (!fpga_rpc(PROTO_CMD_PING, ping_payload, sizeof(ping_payload), response, 1000))
  {
    Serial.println("FPGA ping failed: timeout or invalid frame.");
    return;
  }

  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.print("FPGA ping failed with error code: ");
    if (response.len > 0)
      Serial.println(response.payload[0]);
    else
      Serial.println("unknown");
    return;
  }

  Serial.print("FPGA ping OK, payload: ");
  print_hex_bytes(response.payload, response.len);
  Serial.println();
}

void do_fpga_info()
{
  ProtoFrame response = {};

  if (!fpga_rpc(PROTO_CMD_GET_INFO, nullptr, 0, response, 1000))
  {
    Serial.println("FPGA info failed: timeout or invalid frame.");
    return;
  }

  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.print("FPGA info failed with error code: ");
    if (response.len > 0)
      Serial.println(response.payload[0]);
    else
      Serial.println("unknown");
    return;
  }

  if (response.len < 8)
  {
    Serial.println("FPGA info failed: short payload.");
    return;
  }

  uint16_t max_payload = static_cast<uint16_t>(response.payload[1]) |
                         (static_cast<uint16_t>(response.payload[2]) << 8);

  Serial.print("FPGA protocol version: ");
  Serial.println(response.payload[0]);
  Serial.print("FPGA max payload: ");
  Serial.println(max_payload);
  Serial.print("FPGA feature bits: 0x");
  Serial.println(response.payload[3], HEX);
  Serial.print("FPGA FW version: ");
  Serial.print(response.payload[4]);
  Serial.print(".");
  Serial.println(response.payload[5]);
}

void do_fpga_time()
{
  ProtoFrame response = {};

  if (!fpga_rpc(PROTO_CMD_GET_TIME, nullptr, 0, response, 1000))
  {
    Serial.println("FPGA time failed: timeout or invalid frame.");
    return;
  }

  if (response.msg_type == PROTO_MSG_ERR)
  {
    Serial.print("FPGA time failed with error code: ");
    if (response.len > 0)
      Serial.println(response.payload[0]);
    else
      Serial.println("unknown");
    return;
  }

  if (response.len < 4)
  {
    Serial.println("FPGA time failed: short payload.");
    return;
  }

  uint32_t fpga_time = static_cast<uint32_t>(response.payload[0]) |
                       (static_cast<uint32_t>(response.payload[1]) << 8) |
                       (static_cast<uint32_t>(response.payload[2]) << 16) |
                       (static_cast<uint32_t>(response.payload[3]) << 24);

  Serial.print("FPGA time (hex): 0x");
  Serial.println(fpga_time, HEX);
  Serial.print("FPGA time (dec): ");
  Serial.println(fpga_time);
}

void setup()
{
  Serial.begin(115200);  // Computer <-> Arduino
  Serial1.begin(115200); // Arduino <-> FPGA
  pinMode(led, OUTPUT);

  // Initialize display and touchscreen, then run calibration before
  // waiting on USB serial so the user has something to interact with
  // while the host establishes the CDC connection.
  touch_kb_setup();
  touch_kb_calibrate();

  while (!Serial)
  {
    delay(10);
  }

  Serial.println("REPUSH Dongle Initialized.");

  if (!fetch_ca_key_from_fpga())
  {
    Serial.println("Warning: could not fetch CA key from FPGA. Some features may fail.");
  }
  Serial.println("Commands:");
  Serial.println("*** DEBUG ONLY COMMANDS ***");
  Serial.println("  ready");
  Serial.println("  led_on / led_off");
  Serial.println("  debug_on / debug_off");
  Serial.println("  find_valid");
  Serial.println("  reconfigure <state_index>");
  Serial.println("  keygen <challenge> <state_index>");
  Serial.println("  sign <challenge> <state_index> <nonce> <cookies_b64>");
  Serial.println("  fp_ping");
  Serial.println("  fp_info");
  Serial.println("  fp_time");
  Serial.println("  challenge <c> <state_index> <count> <delay>");
  Serial.println("    state_index: 0-10");
  Serial.println("  challenge choice-puf <tc> <tt> <bc> <bt> <count> <delay>");
  Serial.println("    tt: top_tune (0-7), bt: bottom_tune (0-7)");
  Serial.println("    tc: top_choice (1-3), bc: bottom_choice (0-2)");
  Serial.println("    count: number of reads (e.g., 100)");
  Serial.println("    delay: response delay in ms (e.g., 50)");
  Serial.println("  fp_clear_states");
  Serial.println("  fp_list_states");
  Serial.println("*** PUFMAN <-> DONGLE COMMANDS / PRODUCTION COMMANDS ***");
  Serial.println("  enroll <cert_b64> <payload_b64>");
  Serial.println("    (PUF queried 10x internally; majority-voted seed used for keygen)");
  Serial.println("  acknowledge <domain> <sig_b64>");
  Serial.println("  sign_sensitive <domain> <payload_b64>");
  Serial.println("    payload: base64( \"sensitive\"(9) + C_Session(16) + N(16) + Desc(32) + ServerSig(64) )");
}

void loop()
{
  if (Serial.available())
  {
    String command_str = Serial.readStringUntil('\n');
    command_str.trim();

    Serial.print("**** Received command: '");
    Serial.print(command_str);
    Serial.println("' ****");

    if (command_str.equalsIgnoreCase("led_on"))
    {
      digitalWrite(led, HIGH);
      Serial.println("LED ON");
    }
    else if (command_str.equalsIgnoreCase("ready"))
    {
      Serial.println("READY");
    }
    else if (command_str.equalsIgnoreCase("led_off"))
    {
      digitalWrite(led, LOW);
      Serial.println("LED OFF");
    }
    else if (command_str.equalsIgnoreCase("debug_on"))
    {
      debug_mode = true;
      Serial.println("Debug mode ON");
    }
    else if (command_str.equalsIgnoreCase("debug_off"))
    {
      debug_mode = false;
      Serial.println("Debug mode OFF");
    }
    else if (command_str.startsWith("find_valid"))
    {
      find_valid_challenges();
    }
    else if (command_str.startsWith("reconfigure"))
    {
      int args[1];
      int arg_count = 0;
      int current_pos = command_str.indexOf(' ');

      while (current_pos != -1 && arg_count < 1)
      {
        int next_pos = command_str.indexOf(' ', current_pos + 1);
        String arg_str = (next_pos == -1) ? command_str.substring(current_pos + 1) : command_str.substring(current_pos + 1, next_pos);
        args[arg_count++] = arg_str.toInt();
        current_pos = next_pos;
      }

      if (arg_count == 1)
      {
        int state_index = args[0];

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          reconfigure(state_index);
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: reconfigure <state_index>");
      }
    }
    else if (command_str.startsWith("keygen "))
    {
      int first_space = command_str.indexOf(' ', 7);

      if (first_space != -1)
      {
        int challenge = command_str.substring(7, first_space).toInt();
        int state_index = command_str.substring(first_space + 1).toInt();

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          std::array<uint8_t, 32> priv_key_array = challenge_lr_puf(challenge, state_index, 1, 1);
          uint8_t privateKey[32];
          std::copy(priv_key_array.begin(), priv_key_array.end(), privateKey);

          uint8_t publicKey[32];
          Ed25519::derivePublicKey(publicKey, privateKey);

          Serial.print("PublicKey: ");
          for (int i = 0; i < 32; i++)
          {
            if (publicKey[i] < 16)
              Serial.print("0");
            Serial.print(publicKey[i], HEX);
          }
          Serial.println();
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: keygen <challenge> <state_index>");
      }
    }
    else if (command_str.startsWith("sign "))
    {
      int first_space = command_str.indexOf(' ', 5);
      int second_space = command_str.indexOf(' ', first_space + 1);
      int third_space = command_str.indexOf(' ', second_space + 1);

      if (first_space != -1 && second_space != -1 && third_space != -1)
      {
        int challenge = command_str.substring(5, first_space).toInt();
        int state_index = command_str.substring(first_space + 1, second_space).toInt();
        String nonce = command_str.substring(second_space + 1, third_space);
        String cookies_b64 = command_str.substring(third_space + 1);

        String payload_str = nonce + cookies_b64;

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          std::array<uint8_t, 32> priv_key_array = challenge_lr_puf(challenge, state_index, 1, 1);
          uint8_t privateKey[32];
          std::copy(priv_key_array.begin(), priv_key_array.end(), privateKey);

          uint8_t publicKey[32];
          Ed25519::derivePublicKey(publicKey, privateKey);

          uint8_t signature[64];
          Ed25519::sign(signature, privateKey, publicKey, payload_str.c_str(), payload_str.length());

          Serial.print("Signature: ");
          for (int i = 0; i < 64; i++)
          {
            if (signature[i] < 16)
              Serial.print("0");
            Serial.print(signature[i], HEX);
          }
          Serial.println();
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: sign <challenge> <state_index> <nonce> <cookies_b64>");
      }
    }
    else if (command_str.startsWith("challenge choice-puf "))
    {
      String args_str = command_str.substring(String("challenge choice-puf ").length());
      int top_choice = 0;
      int top_tune = 0;
      int bottom_choice = 0;
      int bottom_tune = 0;
      int count = 0;
      int resp_delay_ms = 50;

      int parsed = sscanf(args_str.c_str(), "%d %d %d %d %d %d",
                          &top_choice, &top_tune, &bottom_choice, &bottom_tune,
                          &count, &resp_delay_ms);

      if (parsed >= 5)
      {
        std::array<uint8_t, 16> majority_response;
        execute_challenge(top_tune, bottom_tune, top_choice, bottom_choice, count, resp_delay_ms, majority_response);
      }
      else
      {
        Serial.println("Error: Invalid 'challenge' command format.");
        Serial.println("Expected: challenge choice-puf <tc> <tt> <bc> <bt> <count> [delay]");
      }
    }
    else if (command_str.startsWith("challenge "))
    {
      int args[4];
      int arg_count = 0;
      int current_pos = command_str.indexOf(' ');

      while (current_pos != -1 && arg_count < 4)
      {
        int next_pos = command_str.indexOf(' ', current_pos + 1);
        String arg_str = (next_pos == -1) ? command_str.substring(current_pos + 1) : command_str.substring(current_pos + 1, next_pos);
        args[arg_count++] = arg_str.toInt();
        current_pos = next_pos;
      }

      if (arg_count == 4)
      {
        int challenge = args[0];
        int state_index = args[1];
        int count = args[2];
        int resp_delay_ms = args[3];

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          challenge_lr_puf(challenge, state_index, count, resp_delay_ms);
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: challenge <c> <state_index> <count> <delay>");
      }
    }
    else if (command_str.equalsIgnoreCase("rc"))
    {
      do_fpga_ping();
    }
    else if (command_str.equalsIgnoreCase("fp_ping"))
    {
      do_fpga_ping();
    }
    else if (command_str.equalsIgnoreCase("fp_info"))
    {
      do_fpga_info();
    }
    else if (command_str.equalsIgnoreCase("fp_time"))
    {
      do_fpga_time();
    }
    else if (command_str.startsWith("enroll "))
    {
      int first_space = command_str.indexOf(' ');
      int second_space = command_str.indexOf(' ', first_space + 1);

      if (second_space == -1)
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: enroll <cert_b64> <payload_b64>");
      }
      else
      {
        String cert_b64 = command_str.substring(first_space + 1, second_space);
        String payload_b64 = command_str.substring(second_space + 1);
        uint8_t cert[CERT_TOTAL_BYTES] = {0};
        uint8_t payload[ENROLL_PAYLOAD_TOTAL_BYTES] = {0};
        size_t cert_len = 0;
        size_t payload_len = 0;

        if (!decode_base64(cert_b64, cert, sizeof(cert), cert_len))
        {
          Serial.println("Error: Invalid cert_b64.");
          return;
        }

        if (!decode_base64(payload_b64, payload, sizeof(payload), payload_len))
        {
          Serial.println("Error: Invalid payload_b64.");
          return;
        }

        if (cert_len != CERT_TOTAL_BYTES)
        {
          Serial.print("Error: cert must decode to ");
          Serial.print(CERT_TOTAL_BYTES);
          Serial.println(" bytes.");
          return;
        }

        if (!ca_pubkey_loaded)
        {
          Serial.println("Error: CA key not available.");
        }
        else if (verify_cert_locally(cert, cert_len))
        {
          Serial.println("CERT_OK");
          print_domain_from_cert(cert);
          print_server_pubkey_from_cert(cert);

          if (verify_enroll_payload(cert, payload, payload_len))
          {
            Serial.println("PAYLOAD_OK");

            // Extract NUL-terminated domain string from the first 32 cert bytes
            char domain[CERT_DOMAIN_BYTES + 1] = {0};
            for (size_t i = 0; i < CERT_DOMAIN_BYTES && cert[i] != 0; i++)
              domain[i] = static_cast<char>(cert[i]);

            if (!touch_kb_confirm_login(domain))
            {
              Serial.println("ENROLL_REJECTED");
            }
            else
            {
              char username[24] = {0};
              char password[24] = {0};
              if (touch_kb_prompt_credentials(username, sizeof(username),
                                              password, sizeof(password)))
              {
                // Step 1: reuse existing slot for this domain, or allocate a free one
                uint8_t state_index = 0;
                {
                  uint8_t _pk[32], _ch[16], _pks[32];
                  if (fpga_puf_find_state_by_domain(domain, state_index, _pk, _ch, _pks))
                  {
                    Serial.println("Note: domain already enrolled, overwriting slot.");
                  }
                  else if (!fpga_puf_get_free_state(state_index))
                  {
                    Serial.println("ENROLL_ERROR: no free PUF state slots");
                    return;
                  }
                }

                // Step 2: reconfigure that state with a fresh random seed
                uint32_t seed = generateRandomSeed();
                if (!fpga_puf_reconfigure_state(state_index, seed))
                {
                  Serial.println("ENROLL_ERROR: PUF state reconfiguration failed");
                  return;
                }

                // Step 3: challenge_id = first 4 bytes of PUF_challenge (payload[5..8], LE)
                uint32_t challenge_id =
                    static_cast<uint32_t>(payload[5]) |
                    (static_cast<uint32_t>(payload[6]) << 8) |
                    (static_cast<uint32_t>(payload[7]) << 16) |
                    (static_cast<uint32_t>(payload[8]) << 24);

                // Step 4: run LR-PUF challenge with per-bit majority voting (ENROLL_PUF_SAMPLES
                // raw measurements voted inside the FPGA) for a stable enrollment seed.
                uint8_t device_privkey[32] = {0};
                if (!fpga_puf_challenge_lr(state_index, challenge_id, device_privkey, ENROLL_PUF_SAMPLES))
                {
                  Serial.println("ENROLL_ERROR: LR-PUF challenge failed");
                  return;
                }

                // Step 5: derive Ed25519 public key
                uint8_t device_pubkey[32];
                Ed25519::derivePublicKey(device_pubkey, device_privkey);

                // Step 6: persist enrollment record on FPGA SD card (acknowledged = 0)
                if (!fpga_puf_set_domain(state_index, domain) ||
                    !fpga_puf_store_enrollment(state_index, device_pubkey,
                                               &payload[5], cert + CERT_DOMAIN_BYTES) ||
                    !fpga_puf_save_states())
                {
                  Serial.println("ENROLL_ERROR: failed to persist state on FPGA");
                  memset(device_privkey, 0, sizeof(device_privkey));
                  return;
                }

                // Step 7: sign the full 101-byte received payload with the device private key
                // (signs "login"||challenge||nonce||server_sig, binding our key to this exact session)
                uint8_t device_sig[64];
                Ed25519::sign(device_sig, device_privkey, device_pubkey,
                              payload, ENROLL_PAYLOAD_TOTAL_BYTES);

                // Step 8: convert server's Ed25519 public key to X25519 for ECIES
                // Done locally via birational map u=(1+y)/(1-y) mod p
                const uint8_t *server_ed25519_pk = cert + CERT_DOMAIN_BYTES;
                uint8_t server_x25519_pk[32];
                ed25519_pk_to_x25519(server_x25519_pk, server_ed25519_pk);

                // Step 9: ECIES — generate an ephemeral X25519 key pair
                uint8_t eph_priv[32];
                fill_random(eph_priv, sizeof(eph_priv));
                uint8_t eph_pub[32];
                Curve25519::dh1(eph_pub, eph_priv); // also clamps eph_priv

                // Step 10: ECDH shared secret
                uint8_t shared[32];
                memcpy(shared, server_x25519_pk, 32);
                Curve25519::dh2(shared, eph_priv);  // shared = X25519(eph_priv, server_x25519_pk)

                // Step 11: derive 32-byte encryption key = SHA256(shared || eph_pub)
                SHA256 sha256;
                uint8_t enc_key[32];
                sha256.reset();
                sha256.update(shared, 32);
                sha256.update(eph_pub, 32);
                sha256.finalize(enc_key, 32);
                memset(shared, 0, sizeof(shared));

                // Step 12: build plaintext "username|password|device_pubkey_hex"
                char plaintext[128] = {0};
                size_t pt_len = 0;
                {
                  size_t ulen = strlen(username);
                  memcpy(plaintext + pt_len, username, ulen);
                  pt_len += ulen;
                  plaintext[pt_len++] = '|';
                  size_t plen = strlen(password);
                  memcpy(plaintext + pt_len, password, plen);
                  pt_len += plen;
                  plaintext[pt_len++] = '|';
                  static const char hex_chars[] = "0123456789ABCDEF";
                  for (size_t i = 0; i < 32; i++)
                  {
                    plaintext[pt_len++] = hex_chars[device_pubkey[i] >> 4];
                    plaintext[pt_len++] = hex_chars[device_pubkey[i] & 0x0F];
                  }
                }

                // Step 13: random 12-byte nonce (IETF ChaCha20-Poly1305)
                uint8_t enc_nonce[12];
                fill_random(enc_nonce, sizeof(enc_nonce));

                // Step 14: ChaCha20-Poly1305 encrypt (12-byte IETF nonce, 16-byte tag)
                ChaChaPoly aead;
                uint8_t ciphertext[128];
                uint8_t tag[16];
                aead.setKey(enc_key, 32);
                aead.setIV(enc_nonce, 12);
                aead.encrypt(ciphertext,
                             reinterpret_cast<const uint8_t *>(plaintext), pt_len);
                aead.computeTag(tag, 16);

                // wipe sensitive material before printing
                memset(enc_key, 0, sizeof(enc_key));
                memset(device_privkey, 0, sizeof(device_privkey));
                memset(eph_priv, 0, sizeof(eph_priv));
                memset(plaintext, 0, sizeof(plaintext));

                // Step 15: print results
                Serial.print("DEVICE_PK: ");
                print_hex_bytes(device_pubkey, 32);
                Serial.println();

                Serial.print("DEVICE_SIG: ");
                print_hex_bytes(device_sig, 64);
                Serial.println();

                // LOGIN_TOKEN: eph_pub(32) || nonce(12) || ciphertext(pt_len) || tag(16)
                // To decrypt: convert server Ed25519 sk -> X25519 sk,
                //   shared = X25519(sk, eph_pub),
                //   key = SHA256(shared || eph_pub),
                //   plaintext = ChaCha20Poly1305(key, nonce).decrypt(ciphertext || tag)
                // Plaintext: "username|password|device_pubkey_hex"
                Serial.print("LOGIN_TOKEN: ");
                print_hex_bytes(eph_pub, 32);
                print_hex_bytes(enc_nonce, 12);
                print_hex_bytes(ciphertext, pt_len);
                print_hex_bytes(tag, 16);
                Serial.println();

                Serial.println("ENROLL_COMPLETE");
              }
              else
              {
                Serial.println("ENROLL_CANCELLED");
              }
            }
          }
          else
            Serial.println("PAYLOAD_BAD");
        }
        else
        {
          Serial.println("CERT_BAD");
        }
      }
    }
    else if (command_str.startsWith("acknowledge "))
    {
      int first_space  = command_str.indexOf(' ');
      int second_space = command_str.indexOf(' ', first_space + 1);

      if (second_space == -1)
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: acknowledge <domain> <sig_b64>");
      }
      else
      {
        String domain_str = command_str.substring(first_space + 1, second_space);
        String sig_b64    = command_str.substring(second_space + 1);

        uint8_t sig[64];
        size_t sig_len = 0;
        if (!decode_base64(sig_b64, sig, sizeof(sig), sig_len) || sig_len != 64)
        {
          Serial.println("ACK_BAD: sig_b64 must decode to exactly 64 bytes.");
        }
        else
        {
          uint8_t state_index = 0;
          uint8_t ack_pubkey[32], ack_challenge[16], ack_pk_server[32];
          if (!fpga_puf_find_state_by_domain(domain_str.c_str(), state_index,
                                             ack_pubkey, ack_challenge, ack_pk_server))
          {
            Serial.println("ACK_BAD: domain not enrolled on this device.");
          }
          else
          {
            // Build the 80-byte message the server signed:
            //   device_pubkey(32) || challenge_raw(16) || domain_padded(32)
            uint8_t ack_msg[80];
            memcpy(ack_msg,      ack_pubkey,   32);
            memcpy(ack_msg + 32, ack_challenge, 16);
            memset(ack_msg + 48, 0, 32);
            size_t dlen = domain_str.length();
            if (dlen > 32) dlen = 32;
            memcpy(ack_msg + 48, domain_str.c_str(), dlen);

            if (!Ed25519::verify(sig, ack_pk_server, ack_msg, sizeof(ack_msg)))
            {
              Serial.println("ACK_BAD: signature verification failed.");
            }
            else if (!fpga_puf_mark_acknowledged(state_index))
            {
              Serial.println("ACK_BAD: failed to persist acknowledgement on FPGA.");
            }
            else
            {
              Serial.println("ACK_OK");
            }
          }
        }
      }
    }
    else if (command_str.startsWith("sign_sensitive "))
    {
      // Phase 2: SignSensitivePayload(Domain, SensitivePayload)
      // Command: sign_sensitive <domain> <payload_b64>
      // Payload decodes to SENSITIVE_PAYLOAD_TOTAL_BYTES (137 bytes):
      //   "sensitive"(9) + C_Session(16) + N(16) + Description(32) + Server_Sig(64)
      int first_space  = command_str.indexOf(' ');
      int second_space = command_str.indexOf(' ', first_space + 1);

      if (second_space == -1)
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: sign_sensitive <domain> <payload_b64>");
      }
      else
      {
        String domain_str   = command_str.substring(first_space + 1, second_space);
        String payload_b64  = command_str.substring(second_space + 1);

        uint8_t payload[SENSITIVE_PAYLOAD_TOTAL_BYTES] = {0};
        size_t  payload_len = 0;

        if (!decode_base64(payload_b64, payload, sizeof(payload), payload_len))
        {
          Serial.println("SIGN_ERROR: Invalid payload_b64.");
        }
        else if (payload_len != SENSITIVE_PAYLOAD_TOTAL_BYTES)
        {
          Serial.print("SIGN_ERROR: payload must decode to ");
          Serial.print(SENSITIVE_PAYLOAD_TOTAL_BYTES);
          Serial.println(" bytes.");
        }
        else if (memcmp(payload, SENSITIVE_PAYLOAD_PREFIX, SENSITIVE_PAYLOAD_PREFIX_BYTES) != 0)
        {
          Serial.println("SIGN_ERROR: payload prefix must be 'sensitive'.");
        }
        else
        {
          // Look up the enrolled state for this domain (provides stored pubkey + pk_server)
          uint8_t state_index = 0;
          uint8_t stored_pubkey[32], stored_challenge[16], pk_server[32];
          if (!fpga_puf_find_state_by_domain(domain_str.c_str(), state_index,
                                             stored_pubkey, stored_challenge, pk_server))
          {
            Serial.println("SIGN_ERROR: domain not enrolled on this device.");
          }
          else
          {
            // Verify server signature on the message portion (first SENSITIVE_PAYLOAD_MSG_BYTES)
            const uint8_t *server_sig = payload + SENSITIVE_PAYLOAD_MSG_BYTES;
            if (!Ed25519::verify(server_sig, pk_server, payload, SENSITIVE_PAYLOAD_MSG_BYTES))
            {
              Serial.println("SIGN_ERROR: server signature verification failed.");
            }
            else
            {
              // Extract C^i_Session (bytes [9..24]) → challenge_id (first 4 bytes LE)
              const uint8_t *c_session = payload + SENSITIVE_PAYLOAD_PREFIX_BYTES;
              uint32_t challenge_id =
                  static_cast<uint32_t>(c_session[0]) |
                  (static_cast<uint32_t>(c_session[1]) << 8) |
                  (static_cast<uint32_t>(c_session[2]) << 16) |
                  (static_cast<uint32_t>(c_session[3]) << 24);

              // Extract NUL-terminated description (bytes [41..72])
              char description[SENSITIVE_DESCRIPTION_BYTES + 1] = {0};
              memcpy(description,
                     payload + SENSITIVE_PAYLOAD_PREFIX_BYTES +
                               SENSITIVE_CHALLENGE_BYTES +
                               SENSITIVE_NONCE_BYTES,
                     SENSITIVE_DESCRIPTION_BYTES);
              description[SENSITIVE_DESCRIPTION_BYTES] = '\0';

              // Show sensitive request on touchscreen; require explicit APPROVE
              if (!touch_kb_confirm_sensitive(domain_str.c_str(), description))
              {
                Serial.println("SIGN_REJECTED");
              }
              else
              {
                // Phase 2 error correction: retry PUF queries until derived PK matches stored PK
                uint8_t device_privkey[32] = {0};
                if (!puf_query_match(state_index, challenge_id, stored_pubkey, device_privkey))
                {
                  Serial.println("SIGN_ERROR: PUF error-correction failed.");
                }
                else
                {
                  // Sign the full 137-byte SensitivePayload with the recovered private key
                  uint8_t device_sig[64];
                  Ed25519::sign(device_sig, device_privkey, stored_pubkey,
                                payload, SENSITIVE_PAYLOAD_TOTAL_BYTES);

                  memset(device_privkey, 0, sizeof(device_privkey));

                  Serial.print("DOMAIN: ");
                  Serial.println(domain_str);
                  Serial.print("DEVICE_SIG: ");
                  print_hex_bytes(device_sig, 64);
                  Serial.println();
                  Serial.println("SIGN_COMPLETE");
                }
              }
            }
          }
        }
      }
    }
    else if (command_str.equalsIgnoreCase("fp_clear_states"))
    {
      if (fpga_puf_clear_states())
        Serial.println("States cleared.");
      else
        Serial.println("Error: failed to clear states.");
    }
    else if (command_str.equalsIgnoreCase("fp_list_states"))
    {
      Serial.println("Slot | Domain                           | Status");
      Serial.println("-----+----------------------------------+-------------");
      for (uint8_t i = 0; i < 11; i++)
      {
        uint8_t is_init = 0, acked = 0;
        char domain[65] = {0};
        if (!fpga_puf_get_slot_status(i, is_init, acked, domain))
        {
          Serial.print("  ");
          Serial.print(i);
          Serial.println("  | (error)");
          continue;
        }
        Serial.print("  ");
        if (i < 10) Serial.print(' ');
        Serial.print(i);
        Serial.print(" | ");
        if (!is_init)
        {
          Serial.println("(empty)");
        }
        else
        {
          size_t dlen = strlen(domain);
          Serial.print(domain);
          for (size_t j = dlen; j < 32; j++) Serial.print(' ');
          Serial.print(" | ");
          Serial.println(acked ? "ACKNOWLEDGED" : "PENDING");
        }
      }
    }
    else if (command_str.length() > 0)
    {
      Serial.print("Unknown command: '");
      Serial.print(command_str);
      Serial.println("'");
    }
  }
}
