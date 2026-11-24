#pragma once

#include <stdint.h>
#include "command_parser.h"
#include "ui/ui_common.h"

extern uint16_t authorized_handle;

void protocol_ble_init(ui_shell_t *shell);
int protocol_ble_get_pairing_code();
uint16_t protocol_ble_get_current_handle();
bool protocol_ble_is_device_connected();
uint8_t *protocol_ble_get_version();
uint8_t *protocol_ble_get_client_version();
void protocol_ble_notify_update_command(wkc_category_t category, const char *name);
void protocol_ble_notify_update_table(wkc_category_t category, const char *name);
void protocol_ble_notify_update_all(wkc_category_t category);
void protocol_ble_set_client_version(const uint8_t *version);
void protocol_ble_reset_client_version();
int protocol_ble_compare_versions(const uint8_t *v1, const uint8_t *v2);
int protocol_ble_compare_native_version(const uint8_t *version);