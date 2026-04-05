#ifndef PUF_FUNCTIONS_H
#define PUF_FUNCTIONS_H

#include <Arduino.h>
#include <vector>
#include <map>

extern bool debug_mode;

std::array<uint8_t, 32> challenge_lr_puf(int challenge, int state_index, int count, int resp_delay_ms);
void build_payload(uint8_t command, uint64_t data_value, uint8_t *payload);
void send_command(const uint8_t *payload);
bool read_response(uint8_t *response, unsigned long timeout = 2000);
uint64_t bytes_to_uint64(const uint8_t *bytes);
void print_binary(uint64_t value, int bits);
void send_setup_and_wait(uint8_t command, uint64_t data_value, unsigned long delay_ms);
bool request_puf_response(uint64_t &puf_value, unsigned long req_delay_ms);
int execute_challenge(int top_tune, int bottom_tune, int top_choice, int bottom_choice, int count, int resp_delay_ms);
void find_valid_challenges();
void reconfigure(int state_index);

#endif // PUF_FUNCTIONS_H
