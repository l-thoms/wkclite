#pragma once

#include "command_parser.h"

typedef enum
{
    PROTOCOL_CHAT_NONE,
    PROTOCOL_CHAT_MESSAGE,
    PROTOCOL_CHAT_VERIFICATION
} protocol_chat_data_type_t;

int protocol_chat_verify(uint16_t conn_handle, wkc_chat_status_t status, uint8_t *data,
    int passkey, int input_length, char *output_data,
    protocol_chat_data_type_t *output_type);
void protocol_chat_init();
void protocol_chat_sessions_register(uint16_t conn_handle, uint8_t *aes);
void protocol_chat_sessions_unregister(uint16_t conn_handle);
bool protocol_chat_is_device_registered(uint16_t conn_handle);