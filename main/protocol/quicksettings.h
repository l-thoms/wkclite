#pragma once

#include <stdint.h>

int protocol_quicksettings_get_current_output(uint8_t *buffer, int request_length);
int protocol_quicksettings_get_table(char *key, bool is_item);
uint8_t protocol_quicksettings_write(char *command);