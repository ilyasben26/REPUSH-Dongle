#pragma once
#include <stddef.h>

// Initialize TFT and touchscreen hardware. Call once from setup().
void touch_kb_setup();

// Two-point calibration: loads saved calibration from flash if available,
// otherwise runs the crosshair sequence and saves the result to flash.
// Must be called after touch_kb_setup().
void touch_kb_calibrate();

// Force a fresh crosshair calibration regardless of saved data, then save.
void touch_kb_force_calibrate();

// Full-screen login confirmation screen showing the target domain.
// Returns true if the user presses ACCEPT, false if they press REJECT.
bool touch_kb_confirm_login(const char *domain);

// Full-screen sensitive-request confirmation screen showing the domain and action description.
// Returns true if the user presses APPROVE, false if they press REJECT.
bool touch_kb_confirm_sensitive(const char *domain, const char *description);

// Full-screen session-renewal confirmation screen.
// Returns true if the user presses APPROVE, false if they press REJECT.
bool touch_kb_confirm_reconf(const char *domain);

// Full-screen username + password prompt.
// Both buffers are NUL-terminated on return.
// Returns true when the user presses DONE for both fields.
// Returns false if the user presses CANCEL on either field.
bool touch_kb_prompt_credentials(char *username, size_t username_max,
                                  char *password, size_t password_max);
