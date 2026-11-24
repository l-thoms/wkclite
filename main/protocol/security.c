#include <sys/time.h>
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "io/io_extend.h"
#include "profile/settings.h"
#include "command_parser.h"
#include "protocol/ble.h"

#define KEY_LENGTH 2048
#define KEY_USAGE PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH | \
        PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT | PSA_KEY_USAGE_EXPORT

static psa_key_id_t rsa_key_id = PSA_KEY_ID_NULL;
static psa_key_id_t aes_key_id = PSA_KEY_ID_NULL;

void protocol_security_init()
{
    ESP_ERROR_CHECK(psa_crypto_init());
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attr, KEY_LENGTH);
    psa_set_key_usage_flags(&attr, KEY_USAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_256));

    ESP_ERROR_CHECK(psa_generate_key(&attr, &rsa_key_id));
}

uint8_t *protocol_security_get_public_key(int *key_length)
{
    size_t key_size = 0;
    uint8_t tmp_data[512];
    psa_export_public_key(rsa_key_id, tmp_data, 512, &key_size);

    uint8_t *key_buffer = calloc(key_size, 1);
    psa_status_t status = psa_export_public_key(rsa_key_id, key_buffer, key_size, &key_size);

    if (status != PSA_SUCCESS)
    {
        ESP_LOGE("PROTOCOL_SECURITY", "Failed to get public key, reason: %d", status);
        return NULL;
    }
    *key_length = (int)key_size;
    return key_buffer;
}

int protocol_security_set_aes(uint8_t *aes)
{
    if (aes_key_id)
    {
        psa_destroy_key(aes_key_id);
        aes_key_id = PSA_KEY_ID_NULL;
    }
    if (!aes) return 0;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);

    psa_status_t status = psa_import_key(&attr, aes, 32, &aes_key_id);
    if (status)
    {
        protocol_security_set_aes(NULL);
    }
    return status;
}

#define VERIFY_ERROR_CHECK(condition, message) do \
{ \
    if (condition) \
    { \
        ESP_LOGE("PROTOCOL_SECURITY", message); \
        ret = 1; \
        goto verify_end; \
    } \
} while (0)

int protocol_security_decrypt_rsa(uint8_t *input, size_t input_length, uint8_t *output,
    size_t output_buffer_length, size_t *output_length)
{
    return psa_asymmetric_decrypt(rsa_key_id, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_256),
                                 input, input_length, NULL, 0, output, output_buffer_length,
                                 output_length);
}

int protocol_security_verify(uint8_t *data, int passkey, int input_length,
                             uint8_t *output_hash)
{
    int64_t system_time = esp_timer_get_time();
    uint8_t output[128] = { 0 };
    size_t output_length;
    uint8_t hash_temp[64] = { 0 };
    uint8_t hash[32] = { 0 };
    uint8_t output_hash_temp[64] = { 0 };
    // Unset is considered as v0.2.0.0 for backward compatibility
    uint8_t current_client_version[4] = { 0, 2, 0, 0 };
    uint8_t *aes;
    int key_length;
    uint8_t *key_buffer = protocol_security_get_public_key(&key_length);
    uint64_t timestamp = 0;
    int ret = protocol_security_decrypt_rsa(data, input_length, output, sizeof(output),
                                            &output_length);

    if (ret)
    {
        ESP_LOGE("PROTOCOL_SECURITY", "Security verify RSA not match, reason: %d",
                 ret);
        goto verify_end;
    }

    memcpy(&timestamp, &output[output_length - 8], 8);
    VERIFY_ERROR_CHECK(!wkc_verify_conn_timestamp(timestamp), "Timestamp not match");

    size_t hash_length;
    psa_hash_compute(PSA_ALG_SHA_256, key_buffer, key_length, hash_temp, 32,
                     &hash_length);

    // New verification
    if (output[0] == 0)
    {
        // 0x00, SHA(SHA(Public key), SHA(Passkey)), Trust key, AES, Client version (v0.2 if unset), Timestamp

        if (output_length == 93)
            memcpy(current_client_version, &output[81], 4);
        else
            VERIFY_ERROR_CHECK(output_length != 89, "Invalid verification message length");

        VERIFY_ERROR_CHECK(protocol_ble_compare_native_version(current_client_version) > 0 ||
            protocol_ble_compare_versions(current_client_version, (uint8_t[])
            { 0, 2, 0, 0 }) < 0, "Unsupported client version");

        // Verify SHA
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)&passkey, 4, &hash_temp[32], 32,
                         &hash_length);
        psa_hash_compute(PSA_ALG_SHA_256, hash_temp, 64, hash, 32,
                         &hash_length);

        VERIFY_ERROR_CHECK(memcmp(hash, &output[1], 32), "Passkey not match");
        aes = &output[49];
        VERIFY_ERROR_CHECK(protocol_security_set_aes(aes), "Cannot set AES");

        wkc_security_append_key(&output[33]);

        // Fill output Hash
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)aes, 32, output_hash_temp,
                         32, &hash_length);
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)&output[33], 16,
                         &output_hash_temp[32], 32, &hash_length);
    }
    else if (output[0] == 1)
    {
        // 0x00, SHA(SHA(Public key), SHA(Trust key)), AES, Client version (v0.2 if unset), Timestamp

        if (output_length == 77)
            memcpy(current_client_version, &output[65], 4);
        else
            VERIFY_ERROR_CHECK(output_length != 73, "Invalid verification message length");

        VERIFY_ERROR_CHECK(protocol_ble_compare_native_version(current_client_version) > 0 ||
            protocol_ble_compare_versions(current_client_version, (uint8_t[])
            { 0, 2, 0, 0 }) < 0, "Unsupported client version");

        // Verify SHA
        bool successed = false;
        uint8_t *current_storage = wkc_security_storage_get_current();
        uint8_t *trust_key = NULL;
        for (int i = 0; i < wkc_get_security_storage_length(); i++)
        {
            uint8_t *currents_storage_item = &current_storage[i * 16];
            psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)currents_storage_item,
                             16, &hash_temp[32], 32, &hash_length);
            psa_hash_compute(PSA_ALG_SHA_256, hash_temp, 64, hash, 32,
                             &hash_length);
            if (memcmp(&output[1], hash, 32) == 0)
            {
                successed = true;
                trust_key = (uint8_t*)currents_storage_item;
                break;
            }
        }
        VERIFY_ERROR_CHECK(!successed, "Trust key not match");

        aes = &output[33];
        VERIFY_ERROR_CHECK(protocol_security_set_aes(aes), "Cannot set AES");

        // Fill output Hash
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)aes, 32, output_hash_temp,
                         32, &hash_length);
        psa_hash_compute(PSA_ALG_SHA_256, trust_key, 16,
                         &output_hash_temp[32], 32, &hash_length);
    }
    else
    {
        ESP_LOGE("PROTOCOL_SECURITY", "Unsupported verification type");
        goto verify_end;
    }

    protocol_ble_set_client_version(current_client_version);

    // Sync time
    wkc_append_conn_timestamp(timestamp);
    timestamp += esp_timer_get_time() - system_time;
    struct tm out_tm;
    timestamp /= 1000000;
    localtime_r((time_t*)&timestamp, &out_tm);
    io_extend_save_time(&out_tm);

    if (output_hash)
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)output_hash_temp,
                         64, output_hash, 32, &hash_length);

    verify_end:
    if (key_buffer)
        free(key_buffer);
    return ret;
}

int protocol_security_decrypt_aes(uint8_t *data, int length,
    uint8_t *result, int *output_length)
{
    if (!data || !result || !output_length)
    {
        *output_length = 0;
        return 1;
    }
    return psa_aead_decrypt(
        aes_key_id, PSA_ALG_GCM, data, 12, NULL, 0, &data[12], length - 12,
        result, length - 28, (size_t*)output_length
    );
}

int protocol_security_encrypt_aes(uint8_t *data, int length,
    uint8_t *result, int *output_length)
{
    if (!data || !result || !output_length)
    {
        *output_length = 0;
        return 1;
    }
    esp_fill_random(result, 12);
    return psa_aead_encrypt(aes_key_id, PSA_ALG_GCM, result, 12, NULL, 0, data,
        length, &result[12], length + 16, (size_t*)output_length);
}

psa_key_id_t protocol_security_get_current_key_id()
{
    return aes_key_id;
}