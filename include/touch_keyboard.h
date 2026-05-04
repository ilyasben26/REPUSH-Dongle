#pragma once
#include <stddef.h>

// Initialize TFT and touchscreen hardware. Call once from setup().
void touch_kb_setup();

// Two-point calibration (blocking): displays crosshairs and waits for the
// user to touch each one. Must be called after touch_kb_setup().
void touch_kb_calibrate();

// Full-screen username + password prompt.
// Both buffers are NUL-terminated on return.
// Returns true when the user presses DONE for both fields.
// Returns false if the user presses CANCEL on either field.
bool touch_kb_prompt_credentials(char *username, size_t username_max,
                                  char *password, size_t password_max);
