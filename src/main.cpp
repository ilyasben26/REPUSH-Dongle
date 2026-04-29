#include <Arduino.h>
#include <Ed25519.h>
#include "puf_functions.h"

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
static constexpr size_t PROTO_MAX_PAYLOAD = 96;

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

bool fpga_rpc(uint8_t cmd, const uint8_t *request_payload, uint16_t request_len, ProtoFrame &response, uint32_t timeout_ms)
{
  static uint8_t seq = 1;
  uint8_t tx_seq = seq++;
  uint32_t start = millis();

  if (request_len > PROTO_MAX_PAYLOAD)
    return false;

  fpga_flush_rx();
  fpga_send_frame(PROTO_MSG_REQ, tx_seq, cmd, request_payload, request_len);

  while ((millis() - start) < timeout_ms)
  {
    uint32_t remaining = timeout_ms - (millis() - start);
    if (!fpga_read_frame(response, remaining))
      return false;

    if (response.seq == tx_seq && response.cmd == cmd)
      return true;
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

  while (!Serial)
  {
    delay(10);
  }

  Serial.println("REPUSH Dongle Initialized.");
  Serial.println("Commands:");
  Serial.println("*** DEBUG ONLY COMMANDS ***");
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
  Serial.println("*** PUFMAN <-> DONGLE COMMANDS / PRODUCTION COMMANDS ***");
  Serial.println("  enroll ");
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
    else if (command_str.length() > 0)
    {
      Serial.print("Unknown command: '");
      Serial.print(command_str);
      Serial.println("'");
    }
  }
}
