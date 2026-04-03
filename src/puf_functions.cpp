#include "puf_functions.h"

struct Challenge
{
    int tc;
    int tt;
    int bc;
    int bt;
};

// --- Pre-calculate the maximum possible number of challenges ---
// (tc, bc) pairs where tc > bc: (1,0), (2,0), (2,1), (3,0), (3,1), (3,2) -> 6 pairs
// Total combinations = 6 pairs * 8 tt values * 8 bt values = 384
const int MAX_POSSIBLE_CHALLENGES = 384;

Challenge *valid_challenges = nullptr;
int num_valid_challenges = 0;
int capacity = 0;

const int PUF_RESPONSE_BITS = 30;
const uint64_t PUF_RESPONSE_MASK = (1ULL << PUF_RESPONSE_BITS) - 1;

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
bool read_response(uint8_t *response, unsigned long timeout)
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
    if (debug_mode)
    {
        Serial.print("[Choice-PUF] Sending command: 0x");
        Serial.print(command, HEX);
        Serial.print(", data: 0x");
        Serial.println(data_value, HEX);
    }
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
int execute_challenge(int top_tune, int bottom_tune, int top_choice, int bottom_choice, int count, int resp_delay_ms)
{

    if (debug_mode)
    {
        Serial.println("**************************************************");
    }

    if (top_choice <= bottom_choice)
    {
        Serial.println("Error: top_choice must be greater than bottom_choice.");
        return -1;
    }

    unsigned long challenge_start = millis();
    const unsigned long setup_delay = 10; // 10ms delay for setup commands

    // 1. Tune top and bottom
    uint64_t tune_val = ((top_tune & 0x7) << 5) | ((bottom_tune & 0x7) << 2);
    if (debug_mode)
    {
        Serial.print("[Choice-PUF] top_tune=");
        Serial.print(top_tune);
        Serial.print(", bottom_tune=");
        Serial.println(bottom_tune);
    }
    send_setup_and_wait(0x2, tune_val, setup_delay);

    // 2. Set choices
    uint64_t choice_val = ((top_choice & 0x3) << 2) | (bottom_choice & 0x3);
    if (debug_mode)
    {
        Serial.print("[Choice-PUF] top_choice=");
        Serial.print(top_choice);
        Serial.print(", bottom_choice=");
        Serial.println(bottom_choice);
    }
    send_setup_and_wait(0x3, choice_val, setup_delay);

    // 3. Determine and set bottom pattern
    uint32_t bottom_pattern = (bottom_choice % 2 == 0) ? 0xAAAAAAAA : 0x55555555;
    int bottom_in_bit = (bottom_choice % 2 == 0) ? 0 : 1;
    uint64_t bottom_payload_val = ((uint64_t)(bottom_in_bit & 0x1) << 32) | bottom_pattern;
    if (debug_mode)
    {
        Serial.print("[Choice-PUF] bottom_pattern=0x");
        Serial.println(bottom_pattern, HEX);
    }
    send_setup_and_wait(0x5, bottom_payload_val, setup_delay);

    // 4. Determine and set top pattern
    uint32_t top_pattern = (top_choice % 2 != 0) ? 0xAAAAAAAA : 0x55555555;
    int top_in_bit = (top_choice % 2 != 0) ? 0 : 1;
    uint64_t top_payload_val = ((uint64_t)(top_in_bit & 0x1) << 32) | top_pattern;
    if (debug_mode)
    {
        Serial.print("[Choice-PUF] top_pattern=0x");
        Serial.println(top_pattern, HEX);
    }
    send_setup_and_wait(0x4, top_payload_val, setup_delay);

    // 5. Query PUF 'count' times
    while (Serial1.available())
        Serial1.read(); // Clear input buffer

    std::vector<int> bit_ones(PUF_RESPONSE_BITS, 0);
    std::map<uint64_t, int> sequence_counts;

    Serial.println("[LR-PUF] Collecting PUF responses...");
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
            Serial.print("[Choice-PUF] Warning: Timeout on response ");
            Serial.println(i + 1);
        }
    }

    if (count == 1)
    {
        Serial.println("[LR-PUF] Single response received:");
        print_binary(sequence_counts.begin()->first, PUF_RESPONSE_BITS);
        Serial.print(" (");
        Serial.print(sequence_counts.begin()->first);
        Serial.println(")");
        return sequence_counts.begin()->first;
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
    Serial.print("[LR-PUF] Majority-voted response (bit mode): ");
    print_binary(majority_value_bit, PUF_RESPONSE_BITS);
    Serial.print(" (");
    Serial.print(majority_value_bit);
    Serial.println(")");

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
    Serial.print("[LR-PUF] Majority-voted response (sequence mode): ");
    print_binary(majority_value_seq, PUF_RESPONSE_BITS);
    Serial.print(" (");
    Serial.print(majority_value_seq);
    Serial.println(")");

    if (debug_mode)
    {
        Serial.println("[LR-PUF] Per-bit summary (% of ones):");
        for (int bit = PUF_RESPONSE_BITS - 1; bit >= 0; bit--)
        {
            float ones_pct = (count > 0) ? (bit_ones[bit] * 100.0f) / count : 0;
            Serial.print("[LR-PUF]  bit[");
            if (bit < 10)
                Serial.print("0");
            Serial.print(bit);
            Serial.print("] -> 1: ");
            Serial.print(ones_pct, 2);
            Serial.println("%");
        }
    }

    unsigned long challenge_elapsed = millis() - challenge_start;
    Serial.print("[LR-PUF] Challenge elapsed time: ");
    Serial.print(challenge_elapsed / 1000.0, 3);
    Serial.println(" s");

    return majority_value_bit; // Return the majority-voted response (bit mode)
}

/***
 * @brief Finds valid challenges (those that don't return all ones) by testing all combinations and pre-allocating a fixed-size array to store results.
 */
void find_valid_challenges()
{
    Serial.println("[LR-PUF] Finding valid challenges...");

    unsigned long start_time = millis();

    if (valid_challenges != nullptr)
    {
        delete[] valid_challenges;
    }

    valid_challenges = new Challenge[MAX_POSSIBLE_CHALLENGES];
    num_valid_challenges = 0;

    int response_delay = 1;
    const long ALL_ONES_30_BIT = 0x3FFFFFFF;

    for (int tc = 1; tc <= 3; tc++)
    {
        for (int tt = 0; tt <= 7; tt++)
        {
            for (int bc = 0; bc <= 2; bc++)
            {
                if (tc > bc)
                {
                    for (int bt = 0; bt <= 7; bt++)
                    {
                        long puf_response = execute_challenge(tt, bt, tc, bc, 1, response_delay);

                        if (puf_response != ALL_ONES_30_BIT)
                        {

                            if (num_valid_challenges < MAX_POSSIBLE_CHALLENGES)
                            {
                                valid_challenges[num_valid_challenges] = {tc, tt, bc, bt};
                                num_valid_challenges++;
                            }
                        }
                    }
                }
            }
        }
    }

    Serial.print("[LR-PUF] Total valid challenges found and stored: ");
    Serial.println(num_valid_challenges);
    Serial.print("[LR-PUF] Allocated array size: ");
    Serial.println(MAX_POSSIBLE_CHALLENGES);

    unsigned long elapsed_time = millis() - start_time;
    Serial.print("[LR-PUF] Valid challenges lookup time: ");
    Serial.print(elapsed_time / 1000.0, 3);
    Serial.println(" s");

    /*
    for (int i = 0; i < num_valid_challenges; i++)
    {
      Serial.print("Stored challenge ");
      Serial.print(i);
      Serial.print(": tc=");
      Serial.print(valid_challenges[i].tc);
      Serial.print(", tt=");
      Serial.print(valid_challenges[i].tt);
      Serial.print(", bc=");
      Serial.print(valid_challenges[i].bc);
      Serial.print(", bt=");
      Serial.println(valid_challenges[i].bt);
    }
    */
}