#include "puf_functions.h"
#include <array>
#include <cstring>
#include <Crypto.h>
#include <SHA256.h>

const uint8_t SEED_PIN = A0;

SHA256 sha256;

struct ChoicePUFChallenge
{
    int tc;
    int tt;
    int bc;
    int bt;
};

const size_t HASH_SIZE = 32;

struct State
{
    unsigned long last_lr_puf_challenge = 0;
    ChoicePUFChallenge last_choice_puf_challenge;
    std::array<uint8_t, HASH_SIZE> hash_value = {0};
    unsigned long last_used_time = 0;
};

const int NUM_STATES = 11;
static State states[NUM_STATES];

static State &get_state(int state_index)
{
    return states[state_index];
}

// --- Pre-calculate the maximum possible number of challenges ---
// (tc, bc) pairs where tc > bc: (1,0), (2,0), (2,1), (3,0), (3,1), (3,2) -> 6 pairs
// Total combinations = 6 pairs * 8 tt values * 8 bt values = 384
const int MAX_POSSIBLE_CHALLENGES = 384;

using PackedChallenge = uint16_t;
static PackedChallenge valid_challenges[MAX_POSSIBLE_CHALLENGES];
int num_valid_challenges = 0;

const int PUF_RESPONSE_BITS = 128;
const int PUF_RESPONSE_BYTES = 16;

ChoicePUFChallenge map_in(int challenge, int state_index);
std::array<uint8_t, 32> map_out(const std::array<uint8_t, 16> &puf_response, int state_index, int challenge);

static PackedChallenge pack_challenge(int tc, int tt, int bc, int bt)
{
    return (PackedChallenge)(((tc & 0x3) << 8) | ((tt & 0x7) << 5) | ((bc & 0x3) << 3) | (bt & 0x7));
}

static ChoicePUFChallenge unpack_challenge(PackedChallenge packed)
{
    ChoicePUFChallenge c;
    c.tc = (packed >> 8) & 0x3;
    c.tt = (packed >> 5) & 0x7;
    c.bc = (packed >> 3) & 0x3;
    c.bt = packed & 0x7;
    return c;
}

static bool is_all_ones_response(const std::array<uint8_t, 16> &value)
{
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] != 0xFF)
        {
            return false;
        }
    }
    return true;
}

static bool get_response_bit(const std::array<uint8_t, 16> &value, int bit)
{
    int byte_index = PUF_RESPONSE_BYTES - 1 - (bit / 8);
    int bit_index = bit % 8;
    return ((value[byte_index] >> bit_index) & 0x1) != 0;
}

static void set_response_bit(std::array<uint8_t, 16> &value, int bit)
{
    int byte_index = PUF_RESPONSE_BYTES - 1 - (bit / 8);
    int bit_index = bit % 8;
    value[byte_index] |= (uint8_t)(1U << bit_index);
}

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
    while (bytes_read < PUF_RESPONSE_BYTES && (millis() - start_time) < timeout)
    {
        if (Serial1.available())
        {
            bytes_read += Serial1.readBytes(response + bytes_read, PUF_RESPONSE_BYTES - bytes_read);
        }
    }
    return bytes_read == PUF_RESPONSE_BYTES;
}

/**
 * @brief Copies a 16-byte big-endian response into an array.
 */
void bytes_to_array16(const uint8_t *bytes, std::array<uint8_t, 16> &out)
{
    memcpy(out.data(), bytes, PUF_RESPONSE_BYTES);
}

void print_binary(uint64_t value, int bits);
void print_binary(const uint8_t *data, size_t size);

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
 * @brief Prints a byte array in binary format.
 */
void print_binary(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        for (int j = 7; j >= 0; --j)
        {
            Serial.print((data[i] >> j) & 1);
        }
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
bool request_puf_response(std::array<uint8_t, 16> &puf_value, unsigned long req_delay_ms)
{
    uint8_t payload[8];
    uint8_t response[PUF_RESPONSE_BYTES];

    build_payload(0x1, 0, payload);
    send_command(payload);
    delay(req_delay_ms);

    if (read_response(response))
    {
        bytes_to_array16(response, puf_value);
        return true;
    }
    return false;
}

/**
 * @brief Executes the Choice PUF challenge sequence.
 */
bool execute_challenge(int top_tune, int bottom_tune, int top_choice, int bottom_choice, int count, int resp_delay_ms, std::array<uint8_t, 16> &majority_response)
{

    majority_response.fill(0);

    if (debug_mode)
    {
        Serial.println("**************************************************");
    }

    if (top_choice <= bottom_choice)
    {
        Serial.println("Error: top_choice must be greater than bottom_choice.");
        return false;
    }
    if (top_tune < 0 || top_tune > 7 || bottom_tune < 0 || bottom_tune > 7)
    {
        Serial.println("Error: top_tune and bottom_tune must be in range 0..7.");
        return false;
    }
    if (top_choice < 0 || top_choice > 3 || bottom_choice < 0 || bottom_choice > 3)
    {
        Serial.println("Error: top_choice and bottom_choice must be in range 0..3.");
        return false;
    }
    if (count <= 0)
    {
        Serial.println("Error: count must be greater than 0.");
        return false;
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

    // 3. Determine and set top pattern
    uint32_t top_pattern = (top_choice % 2 != 0) ? 0xAAAAAAAA : 0x55555555;
    int top_in_bit = (top_choice % 2 != 0) ? 0 : 1;
    uint64_t top_payload_val = ((uint64_t)(top_in_bit & 0x1) << 32) | top_pattern;
    if (debug_mode)
    {
        Serial.print("[Choice-PUF] top_pattern=0x");
        Serial.println(top_pattern, HEX);
    }
    send_setup_and_wait(0x4, top_payload_val, setup_delay);

    // 4. Determine and set bottom pattern
    uint32_t bottom_pattern = (bottom_choice % 2 == 0) ? 0xAAAAAAAA : 0x55555555;
    int bottom_in_bit = (bottom_choice % 2 == 0) ? 0 : 1;
    uint64_t bottom_payload_val = ((uint64_t)(bottom_in_bit & 0x1) << 32) | bottom_pattern;
    if (debug_mode)
    {
        Serial.print("[Choice-PUF] bottom_pattern=0x");
        Serial.println(bottom_pattern, HEX);
    }
    send_setup_and_wait(0x5, bottom_payload_val, setup_delay);

    // 5. Query PUF 'count' times
    while (Serial1.available())
        Serial1.read(); // Clear input buffer

    std::vector<int> bit_ones(PUF_RESPONSE_BITS, 0);
    std::map<std::array<uint8_t, 16>, int> sequence_counts;

    if (debug_mode || count > 1)
    {
        Serial.println("[LR-PUF] Collecting PUF responses...");
    }
    for (int i = 0; i < count; i++)
    {
        std::array<uint8_t, 16> value;
        if (request_puf_response(value, resp_delay_ms))
        {
            for (int bit = 0; bit < PUF_RESPONSE_BITS; bit++)
            {
                if (get_response_bit(value, bit))
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
        if (sequence_counts.empty())
        {
            Serial.println("[Choice-PUF] Error: No response received for single-shot challenge.");
            return false;
        }

        majority_response = sequence_counts.begin()->first;
        Serial.println("[LR-PUF] Single response received:");
        print_binary(majority_response.data(), majority_response.size());
        Serial.println();
        return true;
    }

    // --- Process and print results ---
    std::array<uint8_t, 16> majority_value_bit;
    majority_value_bit.fill(0);
    for (int bit = 0; bit < PUF_RESPONSE_BITS; bit++)
    {
        if (bit_ones[bit] * 2 >= count)
        {
            set_response_bit(majority_value_bit, bit);
        }
    }
    Serial.print("[LR-PUF] Majority-voted response (bit mode): ");
    print_binary(majority_value_bit.data(), majority_value_bit.size());
    Serial.println();

    std::array<uint8_t, 16> majority_value_seq;
    majority_value_seq.fill(0);
    int max_count = 0;
    if (!sequence_counts.empty())
    {
        for (std::map<std::array<uint8_t, 16>, int>::const_iterator it = sequence_counts.begin(); it != sequence_counts.end(); ++it)
        {
            const std::array<uint8_t, 16> &val = it->first;
            int num = it->second;
            if (num > max_count)
            {
                max_count = num;
                majority_value_seq = val;
            }
        }
    }
    Serial.print("[LR-PUF] Majority-voted response (sequence mode): ");
    print_binary(majority_value_seq.data(), majority_value_seq.size());
    Serial.println();

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

    majority_response = majority_value_bit;
    return true;
}

/***
 * @brief Generates a random seed using noise from an unconnected analog pin. Taken from https://rheingoldheavy.com/better-arduino-random-values/
 * @return A 32-bit random seed value.
 */
uint32_t generateRandomSeed()
{
    uint8_t seedBitValue = 0;
    uint8_t seedByteValue = 0;
    uint32_t seedWordValue = 0;

    for (uint8_t wordShift = 0; wordShift < 4; wordShift++) // 4 bytes in a 32 bit word
    {
        for (uint8_t byteShift = 0; byteShift < 8; byteShift++) // 8 bits in a byte
        {
            for (uint8_t bitSum = 0; bitSum <= 8; bitSum++) // 8 samples of analog pin
            {
                seedBitValue = seedBitValue + (analogRead(SEED_PIN) & 0x01); // Flip the coin eight times, adding the results together
            }
            delay(1);                                                             // Delay a single millisecond to allow the pin to fluctuate
            seedByteValue = seedByteValue | ((seedBitValue & 0x01) << byteShift); // Build a stack of eight flipped coins
            seedBitValue = 0;                                                     // Clear out the previous coin value
        }
        seedWordValue = seedWordValue | (uint32_t)seedByteValue << (8 * wordShift); // Build a stack of four sets of 8 coins (shifting right creates a larger number so cast to 32bit)
        seedByteValue = 0;                                                          // Clear out the previous stack value
    }
    return (seedWordValue);
}

/**
 * @brief Reconfigures the state with a new random hash value.
 * This uses the noise from an unconnected analog pin (A0) to seed the random generator.
 */
void reconfigure_state(State &state)
{

    if (state.last_used_time == 0) // S_0 case: initialize with random hash
    {
        Serial.println("[LR-PUF] Initializing state with random hash value...");
        // Seed the random number generator with noise from an unconnected analog pin.
        uint32_t rand32 = generateRandomSeed();
        randomSeed(rand32);
        Serial.print("[LR-PUF] Generated random number for seeding: ");
        Serial.println(rand32);

        int random_index = random(num_valid_challenges);

        Serial.print("[LR-PUF] For S_0, randomly selected challenge index: ");
        Serial.println(random_index);

        ChoicePUFChallenge challenge = unpack_challenge(valid_challenges[random_index]);

        std::array<uint8_t, 16> puf_response;
        if (!execute_challenge(challenge.tt, challenge.bt, challenge.tc, challenge.bc, 1, 10, puf_response))
        {
            Serial.println("[LR-PUF] Error: Failed to collect PUF response for S_0.");
            return;
        }

        Serial.print("[LR-PUF] PUF response for S_0: ");
        print_binary(puf_response.data(), puf_response.size());
        Serial.println();

        // compute the hash of the PUF challenge + response + rand32 + millis
        sha256.reset();
        sha256.update(&challenge.tc, sizeof(challenge.tc));
        sha256.update(&challenge.tt, sizeof(challenge.tt));
        sha256.update(&challenge.bc, sizeof(challenge.bc));
        sha256.update(&challenge.bt, sizeof(challenge.bt));
        sha256.update(puf_response.data(), puf_response.size());
        sha256.update(&rand32, sizeof(rand32));
        unsigned long now = millis();
        sha256.update(&now, sizeof(now));
        sha256.finalize(state.hash_value.data(), state.hash_value.size());

        state.last_choice_puf_challenge = challenge;
        state.last_used_time = now; // millis();
        return;
    }

    // For S_i (i>0)
    uint32_t rand32 = generateRandomSeed();
    randomSeed(rand32);
    Serial.print("[LR-PUF] Generated random number for reconfiguration: ");
    Serial.println(rand32);

    sha256.reset();
    sha256.update(state.hash_value.data(), state.hash_value.size());
    sha256.update(&rand32, sizeof(rand32));
    unsigned long now = millis();
    sha256.update(&now, sizeof(now));
    sha256.finalize(state.hash_value.data(), state.hash_value.size());
    return;
}

void reconfigure(int state_index)
{
    State &state = states[state_index];

    Serial.println("[LR-PUF] Reconfiguring state...");
    Serial.print("[LR-PUF] State index: ");
    Serial.println(state_index);
    Serial.print("[LR-PUF] Current state hash value before reconfiguration: ");
    print_binary(state.hash_value.data(), state.hash_value.size());
    Serial.print(" (");
    for (size_t i = 0; i < state.hash_value.size(); i++)
    {
        Serial.print(state.hash_value[i], HEX);
    }
    Serial.println(")");
    reconfigure_state(state);
    Serial.print("[LR-PUF] State hash value after reconfiguration: ");
    print_binary(state.hash_value.data(), state.hash_value.size());
    Serial.print(" (");
    for (size_t i = 0; i < state.hash_value.size(); i++)
    {
        Serial.print(state.hash_value[i], HEX);
    }
    Serial.println(")");
}

/***
 * @brief Finds valid challenges (those that don't return all ones) by testing all combinations and pre-allocating a fixed-size array to store results.
 */
void find_valid_challenges()
{
    Serial.println("[LR-PUF] Finding valid challenges...");

    unsigned long start_time = millis();

    num_valid_challenges = 0;

    int response_delay = 1;

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
                        std::array<uint8_t, 16> puf_response;
                        bool ok = execute_challenge(tt, bt, tc, bc, 1, response_delay, puf_response);

                        if (ok && !is_all_ones_response(puf_response))
                        {

                            if (num_valid_challenges < MAX_POSSIBLE_CHALLENGES)
                            {
                                valid_challenges[num_valid_challenges] = pack_challenge(tc, tt, bc, bt);
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

std::array<uint8_t, 32> challenge_lr_puf(int challenge, int state_index, int count, int resp_delay_ms)
{
    unsigned long start_time = millis();

    // map the challenge to a valid (tc, tt, bc, bt) choice-puf challenge using the current state
    ChoicePUFChallenge choice_challenge = map_in(challenge, state_index);
    Serial.print("[LR-PUF] Mapped input challenge ");
    Serial.print(challenge);
    Serial.print(" to choice-puf challenge: tc=");
    Serial.print(choice_challenge.tc);
    Serial.print(", tt=");
    Serial.print(choice_challenge.tt);
    Serial.print(", bc=");
    Serial.print(choice_challenge.bc);
    Serial.print(", bt=");
    Serial.println(choice_challenge.bt);

    std::array<uint8_t, 16> puf_response;
    if (!execute_challenge(choice_challenge.tt, choice_challenge.bt, choice_challenge.tc, choice_challenge.bc, count, resp_delay_ms, puf_response))
    {
        Serial.println("[LR-PUF] Error: challenge execution failed.");
        std::array<uint8_t, 32> empty_output = {0};
        return empty_output;
    }

    Serial.print("[LR-PUF] Raw PUF response: ");
    print_binary(puf_response.data(), puf_response.size());
    Serial.println();

    std::array<uint8_t, 32> output = map_out(puf_response, state_index, challenge);

    unsigned long end_time = millis();
    Serial.print("[LR-PUF] Total challenge execution time: ");
    Serial.print((end_time - start_time) / 1000.0, 3);
    Serial.println(" s");

    Serial.print("[LR-PUF] Final output hash for challenge ");
    Serial.print(challenge);
    Serial.print(": ");
    print_binary(output.data(), output.size());
    Serial.print(" (");
    for (size_t i = 0; i < output.size(); i++)
    {
        Serial.print(output[i], HEX);
    }
    Serial.println(")");

    return output;
}

ChoicePUFChallenge map_in(int challenge, int state_index)
{
    State &state = get_state(state_index);

    // 1. Hash the selected state's hash and the external challenge together
    sha256.reset();
    sha256.update(state.hash_value.data(), state.hash_value.size());
    sha256.update((uint8_t *)&challenge, sizeof(challenge));

    std::array<uint8_t, HASH_SIZE> new_hash;
    sha256.finalize(new_hash.data(), new_hash.size());

    uint32_t combined = (uint32_t)new_hash[0] << 24 |
                        (uint32_t)new_hash[1] << 16 |
                        (uint32_t)new_hash[2] << 8 |
                        (uint32_t)new_hash[3];

    if (num_valid_challenges <= 0)
    {
        Serial.println("[LR-PUF] Error: No valid challenges available. Run find_valid first.");
        return {1, 0, 0, 0};
    }

    int valid_index = combined % num_valid_challenges;

    return unpack_challenge(valid_challenges[valid_index]);
}

std::array<uint8_t, 32> map_out(const std::array<uint8_t, 16> &puf_response, int state_index, int challenge)
{
    State &state = get_state(state_index);

    sha256.reset();
    sha256.update(state.hash_value.data(), state.hash_value.size());
    sha256.update((uint8_t *)&challenge, sizeof(challenge));
    sha256.update(puf_response.data(), puf_response.size());

    std::array<uint8_t, HASH_SIZE> output_hash;
    sha256.finalize(output_hash.data(), output_hash.size());
    return output_hash;
}