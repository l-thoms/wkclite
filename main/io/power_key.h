#pragma once

#include <stdbool.h>
#include "ui/shell.h"

void power_key_set_shell(ui_shell_t *shell);
void power_key_init();
// Dispatches one simulated GPIO0 event (used by the serial console)
void power_key_event(bool long_press);
// Enters standby (light sleep) until the key is pressed again
void power_key_standby();
