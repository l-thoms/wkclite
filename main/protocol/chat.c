#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include "esp_random.h"
#include "psa/crypto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ble.h"
#include "command_parser.h"
#include "security.h"
#include "chat.h"
#include "profile/settings.h"

uint64_t chat_timestamp = 0;

typedef struct
{
    uint16_t conn_handle;
    psa_key_id_t key_id;
} chat_session_info_t;

int chat_sessions_length = 0;
static chat_session_info_t chat_sessions[CONFIG_BT_NIMBLE_MAX_CONNECTIONS] = { 0 };
static SemaphoreHandle_t chat_semaphore = NULL;

void protocol_chat_init()
{
    if (!chat_semaphore) chat_semaphore = xSemaphoreCreateMutex();
}

void protocol_chat_sessions_register(uint16_t conn_handle, uint8_t *aes)
{
    xSemaphoreTake(chat_semaphore, portMAX_DELAY);
    for (int i = 0; i < chat_sessions_length; i++)
    {
        if (conn_handle == chat_sessions[i].conn_handle)
        {
            xSemaphoreGive(chat_semaphore);
            return;
        }
    }
    psa_key_id_t key_id;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_status_t status = psa_import_key(&attr, (const uint8_t*)aes, 32, &key_id);
    if (status == 0)
        chat_sessions[chat_sessions_length++] = (chat_session_info_t) {
            .conn_handle = conn_handle,
            .key_id = key_id
        };
    xSemaphoreGive(chat_semaphore);
}

void protocol_chat_sessions_unregister(uint16_t conn_handle)
{
    xSemaphoreTake(chat_semaphore, portMAX_DELAY);
    int stride = 0;
    for (int i = 0; i < chat_sessions_length; i++)
    {
        if (chat_sessions[i].conn_handle == conn_handle)
        {
            if (chat_sessions[i].key_id != PSA_KEY_ID_NULL)
                psa_destroy_key(chat_sessions[i].key_id);
            stride += 1;
        }
        else if (stride > 0)
        {
            memcpy(&chat_sessions[i - stride], &chat_sessions[i],
                sizeof(chat_session_info_t));
        }
    }
    chat_sessions_length -= stride;
    xSemaphoreGive(chat_semaphore);
}

bool protocol_chat_is_device_registered(uint16_t conn_handle)
{
    for (int i = 0; i < chat_sessions_length; i++)
        if (chat_sessions[i].conn_handle == conn_handle) return true;
    return false;
}

psa_key_id_t protocol_chat_get_key_id_from_conn_handle(uint16_t conn_handle)
{
    if (conn_handle == protocol_ble_get_current_handle())
        return protocol_security_get_current_key_id();
    for (int i = 0; i < chat_sessions_length; i++)
    {
        if (chat_sessions[i].conn_handle == conn_handle)
        {
            return chat_sessions[i].conn_handle;
        }
    }
    return PSA_KEY_ID_NULL;
}

#define CHAT_ERROR_CHECK(condition, message) do \
{ \
    if (condition) \
    { \
        ESP_LOGE("PROTOCOL_CHAT", message); \
        ret = 1; \
        goto chat_end; \
    } \
} while (0)

int protocol_chat_verify(uint16_t conn_handle, wkc_chat_status_t status, uint8_t *data,
    int passkey, int input_length, char *output_data,
    protocol_chat_data_type_t *output_type)
{
    uint8_t output[512] = { 0 };
    size_t output_length;
    uint8_t hash_temp[64] = { 0 };
    uint8_t hash[32] = { 0 };
    uint8_t current_client_version[4] = { 0 };
    int key_length;
    uint8_t *key_buffer = NULL;
    uint64_t timestamp = 0;
    int message_length = 0;
    size_t hash_length = 0;
    int ret = 0;
    *output_type = PROTOCOL_CHAT_NONE;

    CHAT_ERROR_CHECK(status == WKC_CHAT_STATUS_DISABLED, "Chat status is disabled");

    key_buffer = protocol_security_get_public_key(&key_length);
    ret = protocol_security_decrypt_rsa(data, input_length, output, sizeof(output),
                                            &output_length);
    output_data = NULL;
    if (ret)
    {
        ESP_LOGE("PROTOCOL_CHAT", "RSA not match, reason: %d",
                 ret);
        goto chat_end;
    }

    CHAT_ERROR_CHECK(output_length < 13, "Data too short");

    memcpy(&timestamp, &output[output_length - 8], 8);
    CHAT_ERROR_CHECK(timestamp < chat_timestamp, "Timestamp not match");

    memcpy(&current_client_version, &output[output_length - 12], 4);
    CHAT_ERROR_CHECK(protocol_ble_compare_native_version(current_client_version) > 0 ||
        protocol_ble_compare_versions(current_client_version, (uint8_t[])
        { 0, 3, 0, 0 }) < 0, "Unsupported client version");
    psa_hash_compute(PSA_ALG_SHA_256, key_buffer, key_length, hash_temp, 32,
                     &hash_length);

    // Open access
    if (output[0] == 0)
    {
        // 0x00, Message, Client version, Timestamp
        message_length = output_length - 13;
        output_data = calloc(message_length + 1, 1);
        memcpy(output_data, &output[1], message_length);
        *output_type = PROTOCOL_CHAT_MESSAGE;
    }
    // Apply for chat-only trust key
    // Chat-only trust key may be appeared var command characteristic
    // Sending message after this application is not allowed
    else if (output[0] == 1)
    {
        CHAT_ERROR_CHECK(output_length != 61, "Unsupported verification length");
        CHAT_ERROR_CHECK(protocol_chat_is_device_registered(conn_handle) ||
                         protocol_ble_get_current_handle() == conn_handle,
            "Device has been registered");
        // 0x01, SHA(SHA(Public key), SHA(Passkey)), Chat-only trust key, Client version (v0.3+), Timestamp
        // Verify SHA
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)&passkey, 4, &hash_temp[32], 32,
                         &hash_length);
        psa_hash_compute(PSA_ALG_SHA_256, hash_temp, 64, hash, 32,
                         &hash_length);

        CHAT_ERROR_CHECK(memcmp(hash, &output[1], 32), "Passkey not match");
        wkc_chat_only_append_key(&output[33]);
    }
    // Limited access phase 1
    // Both chat-only keys and full-control keys are allowed
    // Returns AES(0, SHA(SHA(AES), SHA(Trust Key))) notification for client to verify
    else if (output[0] == 2)
    {
        // 0x02, SHA(SHA(Public Key), SHA(Trust Key)), AES, Client version, Timestamp
        CHAT_ERROR_CHECK(output_length != 61, "Data length not match for limited access verification");
        CHAT_ERROR_CHECK(protocol_chat_is_device_registered(conn_handle) ||
                         protocol_ble_get_current_handle() == conn_handle,
            "Device has been registered");
        message_length = 45;

        // Verify SHA
        bool successed = false;
        uint8_t *trust_key;
        for (int i = 0; i < 2 || successed; i++)
        {
            uint8_t *current_storage = i == 0 ? wkc_security_storage_get_current() :
                                                wkc_chat_only_storage_get_current();
            int storage_length = i == 0 ? wkc_get_security_storage_length() :
                                          wkc_get_chat_only_storage_length();
            for (int j = 0; j < storage_length; j++)
            {
                uint8_t *current_storage_item = &current_storage[j * 16];
                psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)current_storage_item,
                                 16, &hash_temp[32], 32, &hash_length);
                psa_hash_compute(PSA_ALG_SHA_256, hash_temp, 64, hash, 32,
                                 &hash_length);
                if (memcmp(&output[1], hash, 32) == 0)
                {
                    successed = true;
                    trust_key = (uint8_t*)current_storage_item;
                    break;
                }
            }
            if (successed) break;
        }
        CHAT_ERROR_CHECK(!successed, "Trust key not match");

        uint8_t output_hash_temp[64] = { 0 };
        uint8_t output_hash_plain[33] = { 0 };
        uint8_t *aes = &output[33];
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)aes,
                         32, output_hash_temp, 32, &hash_length);
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)trust_key,
                         16, &output_hash_temp[32], 32, &hash_length);


        output_data = calloc(message_length, 1);
        psa_hash_compute(PSA_ALG_SHA_256, output_hash_temp, 64,
                         &output_hash_plain[1], 32, &hash_length);

        protocol_chat_sessions_register(conn_handle, aes);
        psa_key_id_t key_id = protocol_chat_get_key_id_from_conn_handle(conn_handle);
        esp_fill_random(output_data, 12);
        psa_aead_encrypt(key_id, PSA_ALG_GCM, (const uint8_t*)output_data, 12, NULL, 0,
                         output_hash_plain, 33, (uint8_t*)&output_data[12], 33, &output_length);

        *output_type = PROTOCOL_CHAT_VERIFICATION;
    }
    // Limited access phase 2
    // AES is nested into RSA
    else if (output[0] == 3)
    {
        // 0x03, AES(Message), Client version, Timestamp
        CHAT_ERROR_CHECK(output_length < 25, "Data too short for limited access chat");
        message_length = output_length - 25;

        output_data = calloc(message_length + 1, 1);
        psa_status_t decrypt_status =  psa_asymmetric_decrypt(
            protocol_chat_get_key_id_from_conn_handle(conn_handle),
            PSA_ALG_RSA_OAEP(PSA_ALG_SHA_256), &output[1], message_length + 12,
            NULL, 0, (uint8_t*)output_data, message_length, &output_length);
        if (decrypt_status)
        {
            free(output_data);
            ret = 1;
            goto chat_end;
        }
        *output_type = PROTOCOL_CHAT_MESSAGE;
    }
    else
    {
        ESP_LOGE("PROTOCOL_SECURITY", "Unsupported chat type");
        ret = 1;
        goto chat_end;
    }

    chat_timestamp = timestamp;
    chat_end:
    if (key_buffer)
        free(key_buffer);
    return ret;
}