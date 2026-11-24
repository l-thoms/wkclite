#pragma once

#include <stdint.h>
#include "psa/crypto.h"

void protocol_security_init();
int protocol_security_set_aes(uint8_t *aes);
uint8_t *protocol_security_get_public_key(int *key_length);
int protocol_security_verify(uint8_t *data, int passkey, int input_length,
                             uint8_t *output_hash);
int protocol_security_decrypt_aes(uint8_t *data, int length,
    uint8_t *result, int *output_length);
int protocol_security_encrypt_aes(uint8_t *data, int length,
    uint8_t *result, int *output_length);
int protocol_security_decrypt_rsa(uint8_t *input, size_t input_length, uint8_t *output,
    size_t output_buffer_length, size_t *output_length);
psa_key_id_t protocol_security_get_current_key_id();