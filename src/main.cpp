#include <Arduino.h>
#include <vector>
#include <map>

const int PUF_RESPONSE_BITS = 30;
const uint64_t PUF_RESPONSE_MASK = (1ULL << PUF_RESPONSE_BITS) - 1;

int led = LED_BUILTIN;

/**
 * @brief Builds an 8-byte payload for the FPGA.
 */
void build_payload(uint8_t command, uint64_t data_value, uint8_t *payload)
{
  uint64_t payload_value = ((uint64_t)(command & 0xF) << 60) | (data_value & 0x0FFFFFFFFFFFFFFF);
  for (int i = 0; i < 8; ++i)
  {
    payload[i] = (payload_value >> (56 - i * 8)) & 0xFF;
  }
}

/**
 * @brief Sends an 8-byte command payload to the FPGA via Serial1.
 */
void send_command(const uint8_t *payload)
{
  Serial1.write(payload, 8);
  Serial1.flush();
}

/**
 * @brief Reads a 16-byte response from the FPGA via Serial1.
 */
bool read_response(uint8_t *response, unsigned long timeout = 2000)
{
  unsigned long start_time = millis();
  size_t bytes_read = 0;
  while (bytes_read < 16 && (millis() - start_time) < timeout)
  {
    if (Serial1.available())
    {
      bytes_read += Serial1.readBytes(response + bytes_read, 16 - bytes_read);
    }
  }
  return bytes_read == 16;
}

/**
 * @brief Converts the relevant part of a 16-byte response to a 64-bit integer.
 */
uint64_t bytes_to_uint64(const uint8_t *bytes)
{
  uint64_t value = 0;
  for (int i = 8; i < 16; i++)
  {
    value = (value << 8) | bytes[i];
  }
  return value;
}

/**
 * @brief Prints a 64-bit number in binary format.
 */
void print_binary(uint64_t value, int bits)
{
  for (int i = bits - 1; i >= 0; i--)
  {
    Serial.print((value >> i) & 1);
  }
}

/**
 * @brief Sends a setup command and waits for a specific delay.
 */
void send_setup_and_wait(uint8_t command, uint64_t data_value, unsigned long delay_ms)
{
  uint8_t payload[8];
  build_payload(command, data_value, payload);
  send_command(payload);
  delay(delay_ms);
}

/**
 * @brief Sends a PUF request and reads the response.
 */
bool request_puf_response(uint64_t &puf_value, unsigned long req_delay_ms)
{
  uint8_t payload[8];
  uint8_t response[16];

  build_payload(0x1, 0, payload);
  send_command(payload);
  delay(req_delay_ms);

  if (read_response(response))
  {
    puf_value = bytes_to_uint64(response) & PUF_RESPONSE_MASK;
    return true;
  }
  return false;
}

/**
 * @brief Executes the PUF challenge sequence.
 */
void execute_challenge(int top_tune, int bottom_tune, int top_choice, int bottom_choice, int count, int resp_delay_ms)
{
  if (top_choice <= bottom_choice)
  {
    Serial.println("Error: top_choice must be greater than bottom_choice.");
    return;
  }

  unsigned long challenge_start = millis();
  const unsigned long setup_delay = 10; // 10ms delay for setup commands

  // 1. Tune top and bottom
  uint64_t tune_val = ((top_tune & 0x7) << 5) | ((bottom_tune & 0x7) << 2);
  send_setup_and_wait(0x2, tune_val, setup_delay);

  // 2. Set choices
  uint64_t choice_val = ((top_choice & 0x3) << 2) | (bottom_choice & 0x3);
  send_setup_and_wait(0x3, choice_val, setup_delay);

  // 3. Determine and set bottom pattern
  uint32_t bottom_pattern = (bottom_choice % 2 == 0) ? 0xAAAAAAAA : 0x55555555;
  int bottom_in_bit = (bottom_choice % 2 == 0) ? 0 : 1;
  uint64_t bottom_payload_val = ((uint64_t)(bottom_in_bit & 0x1) << 32) | bottom_pattern;
  send_setup_and_wait(0x5, bottom_payload_val, setup_delay);

  // 4. Determine and set top pattern
  uint32_t top_pattern = (top_choice % 2 != 0) ? 0xAAAAAAAA : 0x55555555;
  int top_in_bit = (top_choice % 2 != 0) ? 0 : 1;
  uint64_t top_payload_val = ((uint64_t)(top_in_bit & 0x1) << 32) | top_pattern;
  send_setup_and_wait(0x4, top_payload_val, setup_delay);

  // 5. Query PUF 'count' times
  while (Serial1.available())
    Serial1.read(); // Clear input buffer

  std::vector<int> bit_ones(PUF_RESPONSE_BITS, 0);
  std::map<uint64_t, int> sequence_counts;

  Serial.println("Collecting PUF responses...");
  for (int i = 0; i < count; i++)
  {
    uint64_t value;
    if (request_puf_response(value, resp_delay_ms))
    {
      for (int bit = 0; bit < PUF_RESPONSE_BITS; bit++)
      {
        if ((value >> bit) & 0x1)
        {
          bit_ones[bit]++;
        }
      }
      sequence_counts[value]++;
    }
    else
    {
      Serial.print("Warning: Timeout on response ");
      Serial.println(i + 1);
    }
  }

  // --- Process and print results ---
  uint64_t majority_value_bit = 0;
  for (int bit = 0; bit < PUF_RESPONSE_BITS; bit++)
  {
    if (bit_ones[bit] * 2 >= count)
    {
      majority_value_bit |= (1ULL << bit);
    }
  }
  Serial.print("Majority-voted response (bit mode): ");
  print_binary(majority_value_bit, PUF_RESPONSE_BITS);
  Serial.println();

  uint64_t majority_value_seq = 0;
  int max_count = 0;
  if (!sequence_counts.empty())
  {
    for (auto const &[val, num] : sequence_counts)
    {
      if (num > max_count)
      {
        max_count = num;
        majority_value_seq = val;
      }
    }
  }
  Serial.print("Majority-voted response (sequence mode): ");
  print_binary(majority_value_seq, PUF_RESPONSE_BITS);
  Serial.println();

  Serial.println("Per-bit summary (% of ones):");
  for (int bit = PUF_RESPONSE_BITS - 1; bit >= 0; bit--)
  {
    float ones_pct = (count > 0) ? (bit_ones[bit] * 100.0f) / count : 0;
    Serial.print("  bit[");
    if (bit < 10)
      Serial.print("0");
    Serial.print(bit);
    Serial.print("] -> 1: ");
    Serial.print(ones_pct, 2);
    Serial.println("%");
  }

  unsigned long challenge_elapsed = millis() - challenge_start;
  Serial.print("Challenge elapsed time: ");
  Serial.print(challenge_elapsed / 1000.0, 3);
  Serial.println(" s");
}

void setup()
{
  Serial.begin(115200);
  Serial1.begin(115200);
  pinMode(led, OUTPUT);

  while (!Serial)
  {
    delay(10);
  }

  Serial.println("Arduino PUF Client Initialized.");
  Serial.println("Commands:");
  Serial.println("  puf_req");
  Serial.println("  led_on / led_off");
  Serial.println("  challenge <tt> <bt> <tc> <bc> <count>");
  Serial.println("    tt: top_tune (0-7), bt: bottom_tune (0-7)");
  Serial.println("    tc: top_choice (1-3), bc: bottom_choice (0-2)");
  Serial.println("    count: number of reads (e.g., 100)");
}

void loop()
{
  if (Serial.available())
  {
    String command_str = Serial.readStringUntil('\n');
    command_str.trim();

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
    else if (command_str.equalsIgnoreCase("puf_req"))
    {
      Serial.println("Requesting PUF measurement...");
      uint64_t puf_value;
      if (request_puf_response(puf_value, 50)) // 50ms delay
      {
        Serial.print("PUF Response (HEX): 0x");
        char hex_buffer[9];
        sprintf(hex_buffer, "%08lX", (unsigned long)(puf_value & 0xFFFFFFFF));
        Serial.println(hex_buffer);

        Serial.print("PUF Response (BIN): ");
        print_binary(puf_value, PUF_RESPONSE_BITS);
        Serial.println();
      }
      else
      {
        Serial.println("Error: Timed out waiting for PUF response.");
      }
    }
    else if (command_str.startsWith("challenge"))
    {
      int args[5];
      int arg_count = 0;
      int current_pos = command_str.indexOf(' ');

      while (current_pos != -1 && arg_count < 5)
      {
        int next_pos = command_str.indexOf(' ', current_pos + 1);
        String arg_str = (next_pos == -1) ? command_str.substring(current_pos + 1) : command_str.substring(current_pos + 1, next_pos);
        args[arg_count++] = arg_str.toInt();
        current_pos = next_pos;
      }

      if (arg_count == 5)
      {
        int top_tune = args[0];
        int bottom_tune = args[1];
        int top_choice = args[2];
        int bottom_choice = args[3];
        int count = args[4];
        int resp_delay_ms = 50; // 50ms, matches python --resp-delay default

        execute_challenge(top_tune, bottom_tune, top_choice, bottom_choice, count, resp_delay_ms);
      }
      else
      {
        Serial.println("Error: Invalid 'challenge' command format.");
        Serial.println("Expected: challenge <tt> <bt> <tc> <bc> <count>");
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
