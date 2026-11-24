#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "command_parser.h"
#include "ble.h"
#include "security.h"
#include "chat.h"
#include "profile/userprofile.h"
#include "profile/settings.h"
#include "ui/ui_common.h"
#include "ui/shell.h"
#include "ui/home.h"
#include "security.h"
#include "profile/translations.h"

#define DEVICE_MODEL "WKC Lite"

#define GATT_SVC_UUID 0xA000
#define GATT_CHR_SECURITY_UUID 0xA001   // Read: get public key; write: authenticate
#define GATT_CHR_COMMAND_UUID  0xA002   // Read: get shortcuts; write: execute
#define GATT_CHR_CHAT_UUID  0xA003   // Read: get shortcuts; write: execute
// 0x3b1e stands for 'WKC' (22*26^2+10*26+2=15134=0x3b1e)
#define BLE_GAP_APPEARANCE 0x3b1e
#define BLE_GAP_URI_PREFIX_HTTPS 0x17
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

#define CHAT_DATA_LIMIT 200

// Requested advertising TX power in dBm. BLE_HS_ADV_TX_PWR_LVL_AUTO ("no
// preference") makes this controller fall back to its lowest level (-24 dBm),
// which keeps the advertising packets barely readable; an explicit value is
// required to advertise at a usable power.
#define ADV_TX_POWER_DBM 9

// Maximum advertising data length of the connectable instance. The controller
// only transmits one extended advertising PDU for a connectable set, so all its
// AD structures have to fit into a single PDU (255-byte payload minus the
// extended header, AdvA and ADI).
#define ADV_CONN_MAX_DATA_LEN 245
// Maximum payload of a single TLV block: <length> <id> <content>, so a content
// of 254 bytes still fits the 1-byte length field.
#define ADV_MFG_DATA_MAX 254
// Maximum manufacturer data carried by one AD structure. The AD length octet is
// data length + 1, so this keeps it at 0x7F or below: parsers that read that
// octet as a signed byte misparse larger structures ("Advertising data too
// short").
#define ADV_MFG_BLOCK_DATA_MAX 126
// Maximum total advertising data of the full-data set
// (CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE). Several manufacturer data AD structures
// are chained until this budget is exhausted.
#define ADV_FULL_MAX_DATA_LEN 1650
// Maximum number of manufacturer data blocks: 1650 / 128 + the ones carried by
// the connectable set.
#define ADV_MFG_BLOCK_MAX 16

const uint8_t MFG_HEAD[2] = { 0x1e, 0x3b };
uint8_t protocol_version[4] = { 0, 3, 0, 0 };
uint8_t minimal_supported_version[4] = { 0, 2, 0, 0 };
uint8_t client_version[4] = { 0 };

typedef struct {
    // Actual data length + 1
    uint8_t data_length;
    uint8_t data_type;
    uint8_t *data;
} ble_ext_adv_item_t;

typedef enum {
    BLE_CONNECT_TIME_TOLERANCE_SHORT, // 10s
    BLE_CONNECT_TIME_TOLERANCE_LONG // 30s
} ble_connect_time_tolerance_t;

typedef struct {
    uint16_t conn_handle;
    int64_t last_connect_time_us;
    ble_connect_time_tolerance_t tolerance;
} ble_temporary_device_t;

static ui_shell_t *current_shell;
static uint16_t current_handle = BLE_HS_CONN_HANDLE_NONE;
static int passkey;
SemaphoreHandle_t ble_semaphore = NULL;
static int temporary_devices_count = 0;
static ble_temporary_device_t temporary_devices[CONFIG_BT_NIMBLE_MAX_CONNECTIONS] = { 0 };

typedef struct {
    bool busy;
    bool cancelled;
    SemaphoreHandle_t read_semaphore;
    int read_rc;
    char *name;
} peer_name_ctx_t;

static int ext_adv_calc_size(ble_ext_adv_item_t *items, int count)
{
    int result = 0;
    for (int i = 0; i < count; i++)
        result += items[i].data_length + 2;
    return result;
}

static void ext_adv_build(ble_ext_adv_item_t *items, int count, uint8_t *data)
{
    int data_pos = 0;
    for(int i = 0; i < count; i++)
    {
        data[data_pos] = items[i].data_length + 1;
        data[data_pos + 1] = items[i].data_type;
        memcpy(&data[data_pos + 2], items[i].data, items[i].data_length);
        data_pos += items[i].data_length + 2;
    }
}

// Returns the length of an optional profile value, 0 when it is unset or empty
static int mfg_value_length(const char *value)
{
    return value != NULL ? (int)strlen(value) : 0;
}

// Expected size of the complete manufacturer data stream, kept in sync with
// ext_adv_build_mfg_data(): every value is emitted, an empty one as a zero
// length block.
static int ext_adv_calc_mfg_data_size()
{
    wkc_userprofile_t *profile = wkc_userprofile_get_current();
    wkc_settings_t *settings = wkc_settings_get_current();
    int size = 6; // Head, Protocol version

    size += 2 + mfg_value_length(profile->owner);
    size += 2 + mfg_value_length(profile->character);
    size += 2 + mfg_value_length(profile->manufacturer);
    size += 2 + (int)strlen(DEVICE_MODEL);
    size += 2 + 4; // Minimal supported version
    size += 2 + 1; // Chat status
    size += 2 + ((settings->background_color >= 0 &&
                  settings->background_color <= 0xFFFFFF) ? 3 : 0);
    size += 2 + mfg_value_length(profile->personal_signature);
    size += 2 + mfg_value_length(profile->background_image_url);

    return size;
}

// Format: Head + 4-byte protocol version + TLV Blocks
// Empty data should be indicated explicitly, otherwise the client will not discard
// the previous one
// Head can be any 2-byte values
// TLV Blocks (optional): <data length + 1> <id> <content>
//   Owner: 0x00
//   Character: 0x01
//   Manufacturer: 0x02
//   Model: 0x03,
//   Personal Signature: 0x04
//   Minimal Supported Version: 0x05 (v0.2 if unset)
//   Background Color: 0x06 (3-byte RGB big-endian hex, color will be picked from the
//                     hash of device name if unset)
//   Background Image URL: 0x07
//   Chat Status: 0x08 (0 or unset=Disabled, 1=Verified Only, 2=Open Access)
//   Manufacturer's Specific Data: 0xff (will not be parsed by official WkcCommunicator)
static int ext_adv_append_mfg_block(uint8_t *data, int used, int limit,
                                    wkc_mfg_block_t block, const void *value,
                                    int length)
{
    // <data length + 1> <id> <content>, dropped as a whole when it does not fit
    // into the stream. The 1-byte length field allows up to 254 content bytes;
    // longer blocks are segmented over several advertising structures by
    // ext_adv_pack_mfg_blocks().
    if (length < 0 || length > ADV_MFG_DATA_MAX || used + length + 2 > limit)
        return used;

    data[used] = (uint8_t)(length + 1);
    data[used + 1] = (uint8_t)block;
    if (length > 0)
        memcpy(&data[used + 2], value, length);
    return used + length + 2;
}

// Builds the complete manufacturer data stream (head + protocol version + TLV
// blocks) into `data`, never writing more than `limit` bytes. Trailing TLV
// blocks are dropped when the payload does not fit (truncation happens on TLV
// boundaries, never in the middle of one).
// `brief_length` (optional) receives the stream length up to and including the
// last "brief" block, i.e. the part that goes into the connectable set; the
// signature and the background image URL follow it and belong to the full-data
// set.
// Returns the number of bytes written.
static int ext_adv_build_mfg_data(uint8_t *data, int limit, int *brief_length)
{
    if (limit < 6)
        return 0;

    memcpy(&data[0], MFG_HEAD, 2);
    memcpy(&data[2], protocol_version, 4);
    int used = 6;
    wkc_userprofile_t *profile = wkc_userprofile_get_current();
    wkc_settings_t *settings = wkc_settings_get_current();

    // Every value below is always emitted, an empty one as a zero length block
    // so the client clears the information it cached before
    used = ext_adv_append_mfg_block(data, used, limit, WKC_MFG_BLOCK_OWNER,
                                    profile->owner, mfg_value_length(profile->owner));

    used = ext_adv_append_mfg_block(data, used, limit, WKC_MFG_BLOCK_CHARACTER,
                                    profile->character, mfg_value_length(profile->character));

    used = ext_adv_append_mfg_block(data, used, limit, WKC_MFG_BLOCK_MANUFACTURER,
                                    profile->manufacturer,
                                    mfg_value_length(profile->manufacturer));

    used = ext_adv_append_mfg_block(data, used, limit, WKC_MFG_BLOCK_MODEL,
                                    DEVICE_MODEL, (int)strlen(DEVICE_MODEL));

    used = ext_adv_append_mfg_block(data, used, limit,
                                    WKC_MFG_BLOCK_MINIMAL_SUPPORTED_VERSION,
                                    minimal_supported_version, 4);

    uint8_t color[3] = {
        (uint8_t)(settings->background_color >> 16),
        (uint8_t)((settings->background_color >> 8) & 0xFF),
        (uint8_t)(settings->background_color & 0xFF)
    };
    int color_length = (settings->background_color >= 0 &&
                        settings->background_color <= 0xFFFFFF) ? 3 : 0;
    used = ext_adv_append_mfg_block(data, used, limit,
                                    WKC_MFG_BLOCK_BACKGROUND_COLOR, color, color_length);

    uint8_t chat_status = (uint8_t)settings->chat_status;
    used = ext_adv_append_mfg_block(data, used, limit,
                                    WKC_MFG_BLOCK_CHAT_STATUS, &chat_status, 1);

    // Everything from here on is carried by the full-data set
    if (brief_length != NULL)
        *brief_length = used;

    used = ext_adv_append_mfg_block(data, used, limit,
                                    WKC_MFG_BLOCK_PERSONAL_SIGNATURE,
                                    profile->personal_signature,
                                    mfg_value_length(profile->personal_signature));

    used = ext_adv_append_mfg_block(data, used, limit,
                                    WKC_MFG_BLOCK_BACKGROUND_IMAGE_URL,
                                    profile->background_image_url,
                                    mfg_value_length(profile->background_image_url));

    return used;
}

// Advertising data used by `bytes` bytes of TLV data once packed into blocks.
// The first block repeats the 6-byte head (company id + protocol version) and
// the 2-byte AD header; every following block only carries the 2-byte company
// id plus the AD header.
static int ext_adv_packed_size(int bytes)
{
    if (bytes <= 0)
        return 0;

    int first_capacity = ADV_MFG_BLOCK_DATA_MAX - 6;
    if (bytes <= first_capacity)
        return bytes + 6 + 2;

    int rest = bytes - first_capacity;
    int rest_capacity = ADV_MFG_BLOCK_DATA_MAX - 2;
    int blocks = (rest + rest_capacity - 1) / rest_capacity;
    return bytes + 6 + 2 + blocks * (2 + 2);
}

// Packs the manufacturer data stream [start, end) into advertising blocks.
// Only the first block of the connectable set declares the 4-byte protocol
// version: it starts with the full 6-byte head (company id + version), while
// every following block starts with the 2-byte company id only. A block carries
// at most ADV_MFG_BLOCK_DATA_MAX bytes including its header, so it never exceeds
// the 126-byte payload limit. A TLV that does not fit into the current block is
// continued in the next block of the same advertising set; the scanner
// concatenates the block payloads in transmission order. The caller keeps
// [start, end) inside a single advertising set, so a TLV is never split between
// the connectable and the full-data set.
// `max_ad_total` bounds the total advertising data used by the blocks.
// Returns the stream offset that has been packed (== end when everything fit).
static int ext_adv_pack_mfg_blocks(const uint8_t *stream, int start, int end,
                                   int max_ad_total, uint8_t *arena,
                                   int *block_lengths, int *block_count,
                                   int max_blocks)
{
    int pos = start;
    int ad_total = 0;

    while (pos < end && *block_count < max_blocks)
    {
        // Only the very first block declares the protocol version
        int header = (*block_count == 0) ? 6 : 2;
        int chunk = end - pos;
        if (chunk > ADV_MFG_BLOCK_DATA_MAX - header)
            chunk = ADV_MFG_BLOCK_DATA_MAX - header;
        if (ad_total + 2 + header + chunk > max_ad_total)
            chunk = max_ad_total - ad_total - 2 - header;
        if (chunk <= 0)
            break;

        uint8_t *block = &arena[*block_count * ADV_MFG_BLOCK_DATA_MAX];
        memcpy(block, stream, header);
        memcpy(&block[header], &stream[pos], chunk);
        block_lengths[*block_count] = header + chunk;
        (*block_count)++;
        ad_total += 2 + header + chunk;
        pos += chunk;
    }

    return pos;
}

static void temporary_device_append(uint16_t conn_handle)
{
    xSemaphoreTake(ble_semaphore, portMAX_DELAY);
    int64_t current_time = esp_timer_get_time();
    for (int i = 0; i < temporary_devices_count; i++)
    {
        if(temporary_devices[i].conn_handle == conn_handle)
        {
            xSemaphoreGive(ble_semaphore);
            return;
        }
    }
    temporary_devices[temporary_devices_count] = (ble_temporary_device_t) {
        .conn_handle = conn_handle,
        .last_connect_time_us=current_time,
        .tolerance = BLE_CONNECT_TIME_TOLERANCE_SHORT
    };
    temporary_devices_count += 1;
    xSemaphoreGive(ble_semaphore);
}

static void temporary_device_set_tolerance(uint16_t conn_handle,
    ble_connect_time_tolerance_t tolerance)
{
    xSemaphoreTake(ble_semaphore, portMAX_DELAY);
    for (int i = 0; i < temporary_devices_count; i++)
    {
        if (temporary_devices[i].conn_handle == conn_handle)
            temporary_devices[i].tolerance = tolerance;
    }
    xSemaphoreGive(ble_semaphore);
}

static void temporary_device_remove(uint16_t conn_handle)
{
    xSemaphoreTake(ble_semaphore, portMAX_DELAY);
    int stride = 0;
    for (int i = 0; i < temporary_devices_count; i++)
    {
        if (temporary_devices[i].conn_handle == conn_handle) stride += 1;
        else if (stride > 0)
            memcpy(&temporary_devices[i - stride], &temporary_devices[i],
                   sizeof(ble_temporary_device_t));
    }
    temporary_devices_count -= stride;
    xSemaphoreGive(ble_semaphore);
}

static int gatt_svr_security_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc = 0;
    uint8_t notify_rc[33] = { 0 };
    struct os_mbuf *om = ctxt->om;
    int om_length = os_mbuf_len(om);
    uint16_t output_om_length;
    uint8_t *om_data = calloc(om_length, 1);
    ble_hs_mbuf_to_flat(om, om_data, om_length, &output_om_length);

    switch (ctxt->op)
    {
        // Read public key
        case BLE_GATT_ACCESS_OP_READ_CHR:
        {
            int key_length;
            uint8_t *key_buffer = protocol_security_get_public_key(&key_length);

            passkey = esp_random() % 1000000;
            temporary_device_set_tolerance(conn_handle, BLE_CONNECT_TIME_TOLERANCE_LONG);
            ESP_LOGI("WKC_BLE", "Passkey: %06d", passkey);
            if (passkey >= 0)
            {
                char *verification = wkc_translations_get_string("ble_passkey");
                char *message = (char*)calloc(strlen(verification) + 7, 1);
                sprintf(message, "%s%06d", verification, passkey);
                ui_shell_show_toast(current_shell, message, 30);
                free(message);
            }

            rc = os_mbuf_append(om, key_buffer, key_length);
            free(key_buffer);
        }
        break;
        // Verify
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
        {
            struct os_mbuf *notify_om = ble_hs_mbuf_att_pkt();
            if (current_handle != BLE_HS_CONN_HANDLE_NONE)
            {
                ui_shell_show_toast(current_shell,
                    wkc_translations_get_string("ble_disconnect_previous_device"), 5);
                rc = (int)WKC_NOTIFY_CONNECTION_OCCUPIED;
            }
            else if (protocol_chat_is_device_registered(conn_handle))
            {
                ui_shell_show_toast(current_shell,
                    wkc_translations_get_string("ble_exit_chat_session"), 5);
                rc = (int)WKC_NOTIFY_FAILED;
            }
            else if (passkey >= 0 &&
                     protocol_security_verify(om_data, passkey, om_length,
                     &notify_rc[1]) == 0)
            {
                temporary_device_remove(conn_handle);
                ui_shell_show_toast(current_shell,
                    wkc_translations_get_string("ble_verification_success"), 5);
                ui_home_update_from_shell(current_shell);
                current_handle = conn_handle;
                ESP_LOGI("WKC_BLE", "Device verify okay");
                rc = (int)WKC_NOTIFY_SUCCESSED;
            }
            else
            {
                ui_shell_show_toast(current_shell,
                    wkc_translations_get_string("ble_verification_failed"), 5);
                passkey = -1;
                rc = (int)WKC_NOTIFY_FAILED;
            }

            int rc_packed_length;
            // Returns AES(0, SHA(SHA(AES), SHA(Trust Key))) for 0.3+ client,
            // AES(0)for 0.2 client
            if (rc == WKC_NOTIFY_SUCCESSED)
            {
                bool legacy = protocol_ble_compare_versions(
                    protocol_ble_get_client_version(), (uint8_t[]) { 0, 2, 0, 0 }) <= 0;
                uint8_t *rc_packed = wkc_command_pack(notify_rc, legacy ? 1 : 33,
                    &rc_packed_length);
                if (rc_packed)
                {
                    os_mbuf_append(notify_om, rc_packed, rc_packed_length);
                    free(rc_packed);
                }
            }
            else
            {
                os_mbuf_append(notify_om, &rc, 1);
            }
            ble_gatts_notify_custom(conn_handle, attr_handle, notify_om);
            os_mbuf_free_chain(notify_om);

            // Disconnect after failed
            if (rc)
            {
                ble_gap_terminate(conn_handle, 0x05);
            }
        }
        break;
        default:
        {
            rc = BLE_ATT_ERR_UNLIKELY;
        }
        break;
    }

    free(om_data);
    return rc;
}

static int gatt_svr_command_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;
    struct os_mbuf *om = ctxt->om;
    switch (ctxt->op)
    {
        case BLE_GATT_ACCESS_OP_READ_CHR:
        {
            if (conn_handle != current_handle)
            {
                ESP_LOGE("WKC_BLE", "Read permission denided.");
                return BLE_ATT_ERR_READ_NOT_PERMITTED;
            }
            int command_output_length = 0;
            uint8_t *command_output = wkc_get_command_output(&command_output_length);
            if(!command_output)
            {
                ESP_LOGE("WKC_BLE", "Failed to read command output");
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            else
            {
                rc = os_mbuf_append(om, command_output, command_output_length);
                if(rc != 0)
                    ESP_LOGE("WKC_BLE", "Failed to send result");
                free(command_output);
                return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
        }
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
        {
            if (conn_handle != current_handle)
            {
                ESP_LOGE("WKC_BLE", "Write permission denided.");
                return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
            }
            // Write characteristics
            uint16_t command_length = os_mbuf_len(om);
            int command_length_out;
            uint8_t *command_raw = calloc(command_length + 1, 1);
            ble_hs_mbuf_to_flat(om, command_raw, command_length,
                                (uint16_t*)&command_length_out);
            uint8_t *command = wkc_command_unpack(command_raw, command_length,
                               &command_length_out);
            if (command)
            {
                wkc_write_command(conn_handle, attr_handle, command,
                                  command_length_out, current_shell);
                free(command);
            }
            else
            {
                wkc_write_command(conn_handle, attr_handle, NULL, 0, current_shell);
            }
            free(command_raw);
            return 0;
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

static int read_peer_name_cb(uint16_t conn_handle, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg)
{
    peer_name_ctx_t *ctx = (peer_name_ctx_t *)arg;
    ctx->read_rc = err->status;
    if (err->status == 0 && attr && attr->om && !ctx->cancelled)
    {
        uint16_t length = os_mbuf_len(attr->om);
        ctx->name = calloc(length + 1, 1);
        os_mbuf_copydata(attr->om, 0, length, ctx->name);
        xSemaphoreGive(ctx->read_semaphore);
    }
    ctx->busy = false;
    return 0;
}

static char *read_peer_name(uint16_t conn_handle)
{
    static peer_name_ctx_t current_peer_name_ctx = { 0 };
    int rc = 0;
    if (current_peer_name_ctx.busy)
    {
        rc = 1;
        ESP_LOGE("WKC_BLE", "Read peer name is busy");
        goto peer_name_release;
    }

    current_peer_name_ctx.busy = true;
    if (!current_peer_name_ctx.read_semaphore)
        current_peer_name_ctx.read_semaphore = xSemaphoreCreateBinary();

    current_peer_name_ctx.cancelled = false;
    rc = ble_gattc_read_by_uuid(conn_handle, 1, 0xFFFF, BLE_UUID16_DECLARE(0x2A00),
                                read_peer_name_cb, &current_peer_name_ctx);
    if (rc)
    {
        ESP_LOGE("WKC_BLE", "Failed to read peer name, reason: %d", rc);
        current_peer_name_ctx.busy = false;
        goto peer_name_release;
    }
    if (xSemaphoreTake(current_peer_name_ctx.read_semaphore, 5000) != pdTRUE)
    {
        rc = 1;
        current_peer_name_ctx.cancelled = true;
        ESP_LOGE("WKC_BLE", "Read peer name time out");
        goto peer_name_release;
    }
    else
        rc = current_peer_name_ctx.read_rc;

    peer_name_release:
    return rc ? NULL : current_peer_name_ctx.name;
}

static int gatt_svr_chat_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc = 0;
    struct os_mbuf *om = ctxt->om;
    int om_length = os_mbuf_len(om);
    uint16_t output_om_length;
    uint8_t *om_data = calloc(om_length, 1);
    ble_hs_mbuf_to_flat(om, om_data, om_length, &output_om_length);
    wkc_settings_t *settings = wkc_settings_get_current();
    switch (ctxt->op)
    {
        // Read public key sliently
        case BLE_GATT_ACCESS_OP_READ_CHR:
        {
            int key_length;
            uint8_t *key_buffer = protocol_security_get_public_key(&key_length);
            rc = os_mbuf_append(om, key_buffer, key_length);
            free(key_buffer);
        }
        break;
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
        {
            struct os_mbuf *notify_om = ble_hs_mbuf_att_pkt();
            protocol_chat_data_type_t output_type;
            char *chat_data = NULL;
            if (protocol_chat_verify(conn_handle, settings->chat_status, om_data,
                settings->chat_passkey, om_length, chat_data, &output_type) == 0)
            {
                switch (output_type)
                {
                    case PROTOCOL_CHAT_MESSAGE:
                        if (chat_data)
                        {
                            char *peer_name = read_peer_name(conn_handle);
                            char *selected_name = peer_name ? peer_name :
                                 wkc_translations_get_string("ble_message_unknown_device");
                            size_t chat_data_length = strlen(chat_data);
                            bool exceed = chat_data_length > CHAT_DATA_LIMIT;
                            if (exceed)
                            {
                                char *chat_data_cropped = font_crop_text(chat_data,
                                    CHAT_DATA_LIMIT);
                                free(chat_data);
                                chat_data = chat_data_cropped;
                                chat_data_length = strlen(chat_data);
                            }
                            int chat_concat_length = chat_data_length +
                                strlen(selected_name) +
                                strlen(wkc_translations_get_string("ble_message_from_format") + 4);
                            char *chat_concat_data = calloc(chat_concat_length, 1);
                            sprintf(chat_concat_data,
                                wkc_translations_get_string("ble_message_from_format"),
                                selected_name, chat_data);
                            ui_shell_show_toast(ui_shell_get_current(), chat_concat_data,
                                clamp((int)(chat_data_length / 5.f + 0.5f), 10, 30));
                            if (peer_name) free(peer_name);
                        }
                        os_mbuf_append(notify_om, (uint8_t[]) { 0 }, 1);
                        break;
                    case PROTOCOL_CHAT_VERIFICATION:
                        os_mbuf_append(notify_om, (const uint8_t*)chat_data, 45);
                        break;
                    default:
                        os_mbuf_append(notify_om, (uint8_t[]) { 0 }, 1);
                        break;
                }
                if (chat_data) free(chat_data);
                ble_gatts_notify_custom(conn_handle, attr_handle, notify_om);
                ble_gap_terminate(conn_handle, 0x16);
            }
            else
            {
                os_mbuf_append(notify_om, (uint8_t[]) { 1 }, 1);
                ble_gatts_notify_custom(conn_handle, attr_handle, notify_om);
                ble_gap_terminate(conn_handle, 0x05);
            }
            os_mbuf_free_chain(notify_om);
        }
        break;
        default:
            rc = BLE_ATT_ERR_UNLIKELY;
    }

    free(om_data);
    return rc;
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);

static int set_adv_fields()
{
    int rc = 0;
    uint8_t *mfg_stream = NULL, *mfg_arena = NULL,
            *ext_adv_data = NULL, *ext_adv_data_full = NULL;
    struct os_mbuf *ext_adv_mbuf = NULL, *ext_adv_full_mbuf = NULL;
    wkc_userprofile_t *profile = wkc_userprofile_get_current();

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0)
    {
        ESP_LOGE("WKC_BLE", "device does not have any available bt address, reason: %d", rc);
        goto set_adv_fields_end;
    }

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc)
    {
        ESP_LOGE("WKC_BLE", "Cannot determine addr_type, reason: %d", rc);
        goto set_adv_fields_end;
    }

    // Instance 0 is the connectable set, instance 1 carries the complete
    // manufacturer data. Instance 1 keeps running independently of the
    // connection, so both have to be stopped before being reconfigured;
    // otherwise the configure call below fails with BLE_HS_EBUSY when
    // advertising is restarted.
    for (uint8_t i = 0; i < 2; i++)
    {
        if (ble_gap_ext_adv_active(i))
            ble_gap_ext_adv_stop(i);
    }

    // Configure extended advertising
    struct ble_gap_ext_adv_params adv_params = {
        .sid = 0,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .tx_power = ADV_TX_POWER_DBM,
        .connectable = 1,
    };
    struct ble_gap_ext_adv_params adv_params_full = {
        // Non-connectable and non-scannable: the complete manufacturer data is
        // carried in the advertising data (AUX_ADV_IND), so it reaches every
        // scanner without depending on (extended) active scanning. A scannable
        // set can only deliver the payload in the AUX_SCAN_RSP, which requires
        // the scanner to send a scan request first -- many stacks (Windows
        // confirmed) never do that for extended advertising.
        .sid = 1,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .tx_power = ADV_TX_POWER_DBM,
    };
    int8_t selected_tx_power = 0, selected_tx_power_full = 0;
    rc = ble_gap_ext_adv_configure(0, &adv_params, &selected_tx_power,
                                   ble_gap_event_handler, NULL);
    if (rc != 0)
    {
        ESP_LOGE("WKC_BLE", "Failed to configure ext adv, reason: %d", rc);
        goto set_adv_fields_end;
    }
    rc = ble_gap_ext_adv_configure(1, &adv_params_full, &selected_tx_power_full,
                                   NULL, NULL);
    if (rc != 0)
    {
        ESP_LOGE("WKC_BLE", "Failed to configure full data ext adv, reason: %d", rc);
        goto set_adv_fields_end;
    }
    ESP_LOGI("WKC_BLE", "Advertising TX power: connectable %d dBm, full %d dBm",
             selected_tx_power, selected_tx_power_full);

    // Build advertise data
    uint8_t adv_flags_data = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    uint8_t role = BLE_GAP_LE_ROLE_PERIPHERAL;
    uint16_t appearance = BLE_GAP_APPEARANCE;

    // The connectable set has to stay within a single advertising PDU, so its
    // manufacturer data is only an excerpt that fits next to the mandatory AD
    // structures (flags, complete name, appearance and LE role). The 2-byte AD
    // header of every manufacturer data structure is accounted for separately,
    // one per block.
    int device_name_length = (int)strlen(profile->device_name);
    int conn_overhead = 3 /* flags */ + (2 + device_name_length) +
                        4 /* appearance */ + 3 /* LE role */;
    int conn_mfg_limit = ADV_CONN_MAX_DATA_LEN - conn_overhead;
    if (conn_mfg_limit < 6)
        conn_mfg_limit = 6;

    // Build the complete manufacturer data stream and split it into blocks.
    // Every block repeats the 6-byte header and continues the stream where the
    // previous one stopped, so no data is advertised twice: the leading blocks
    // are carried by the connectable set, the remaining ones by the full-data
    // set.
    mfg_stream = calloc(ADV_FULL_MAX_DATA_LEN, 1);
    mfg_arena = calloc(ADV_MFG_BLOCK_MAX * ADV_MFG_BLOCK_DATA_MAX, 1);
    if (!mfg_stream || !mfg_arena)
    {
        ESP_LOGE("WKC_BLE", "Failed to allocate manufacturer data.");
        goto set_adv_fields_end;
    }

    int mfg_brief_length = 0;
    int mfg_stream_length = ext_adv_build_mfg_data(mfg_stream, ADV_FULL_MAX_DATA_LEN,
                                                   &mfg_brief_length);

    // The connectable set carries the brief blocks (owner, character,
    // manufacturer, model, versions, color and chat status); the signature and
    // the background image URL belong to the full-data set. The boundary is a
    // TLV boundary, so one TLV is never split between the two sets.
    int main_boundary = 6;
    while (main_boundary < mfg_brief_length)
    {
        int tlv_total = 1 + mfg_stream[main_boundary];
        if (mfg_stream[main_boundary] == 0 ||
            main_boundary + tlv_total > mfg_brief_length)
            break;
        if (ext_adv_packed_size(main_boundary + tlv_total - 6) > conn_mfg_limit)
            break;
        main_boundary += tlv_total;
    }

    int block_lengths[ADV_MFG_BLOCK_MAX];
    int block_count = 0;
    ext_adv_pack_mfg_blocks(mfg_stream, 6, main_boundary, conn_mfg_limit,
                            mfg_arena, block_lengths, &block_count,
                            ADV_MFG_BLOCK_MAX);
    int main_block_count = block_count;
    ext_adv_pack_mfg_blocks(mfg_stream, main_boundary, mfg_stream_length,
                            ADV_FULL_MAX_DATA_LEN, mfg_arena, block_lengths,
                            &block_count, ADV_MFG_BLOCK_MAX);
    int full_block_count = block_count - main_block_count;

    if (main_block_count == 0 && full_block_count == 0)
    {
        ESP_LOGE("WKC_BLE", "Failed to build manufacturer data blocks.");
        goto set_adv_fields_end;
    }
    // Nothing left for the second set: keep it alive with a company-id-only
    // block (the protocol version is only declared by the first block)
    if (full_block_count == 0)
    {
        uint8_t *block = &mfg_arena[block_count * ADV_MFG_BLOCK_DATA_MAX];
        memcpy(block, mfg_stream, 2);
        block_lengths[block_count] = 2;
        block_count++;
        full_block_count = 1;
    }

    ESP_LOGI("WKC_BLE", "MFG: %d/%d B, brief %d B, blocks %d main + %d full",
             mfg_stream_length, ext_adv_calc_mfg_data_size(), main_boundary,
             main_block_count, full_block_count);

    ble_ext_adv_item_t ext_adv_items[4 + ADV_MFG_BLOCK_MAX];
    int item_count = 0;
    {
        ext_adv_items[item_count++] = (ble_ext_adv_item_t) {
            .data_type = BLE_HS_ADV_TYPE_FLAGS,
            .data_length = 1,
            .data = &adv_flags_data
        };
        ext_adv_items[item_count++] = (ble_ext_adv_item_t) {
            .data_type = BLE_HS_ADV_TYPE_COMP_NAME,
            .data_length = strlen(profile->device_name),
            .data = (uint8_t*)profile->device_name
        };
        ext_adv_items[item_count++] = (ble_ext_adv_item_t) {
            .data_type = BLE_HS_ADV_TYPE_APPEARANCE,
            .data_length = 2,
            .data = (uint8_t*)&appearance
        };
        ext_adv_items[item_count++] = (ble_ext_adv_item_t) {
            .data_type = BLE_HS_ADV_TYPE_LE_ROLE,
            .data_length = 1,
            .data = &role
        };
    }
    for (int i = 0; i < main_block_count; i++)
    {
        ext_adv_items[item_count++] = (ble_ext_adv_item_t) {
            .data_type = BLE_HS_ADV_TYPE_MFG_DATA,
            .data_length = (uint8_t)block_lengths[i],
            .data = &mfg_arena[i * ADV_MFG_BLOCK_DATA_MAX]
        };
    }

    // One AD structure per remaining block, in transmission order
    ble_ext_adv_item_t ext_adv_items_full[ADV_MFG_BLOCK_MAX];
    int item_count_full = 0;
    for (int i = main_block_count; i < block_count; i++)
    {
        ext_adv_items_full[item_count_full] = (ble_ext_adv_item_t) {
            .data_type = BLE_HS_ADV_TYPE_MFG_DATA,
            .data_length = (uint8_t)block_lengths[i],
            .data = &mfg_arena[i * ADV_MFG_BLOCK_DATA_MAX]
        };
        item_count_full++;
    }
    int ext_adv_size = ext_adv_calc_size(ext_adv_items, item_count);
    int ext_adv_size_full = ext_adv_calc_size(ext_adv_items_full, item_count_full);
    ext_adv_data = calloc(ext_adv_size, 1);
    ext_adv_data_full = calloc(ext_adv_size_full, 1);
    ext_adv_build(ext_adv_items, item_count, ext_adv_data);
    ext_adv_build(ext_adv_items_full, item_count_full, ext_adv_data_full);

    ESP_LOGI("WKC_BLE", "Advertising data: connectable %d B, full %d B",
             ext_adv_size, ext_adv_size_full);

    // ble_hs_mbuf_from_flat() chains mbufs as needed, so it also works for
    // payloads that do not fit into a single msys block.
    ext_adv_mbuf = ble_hs_mbuf_from_flat(ext_adv_data, ext_adv_size);
    ext_adv_full_mbuf = ble_hs_mbuf_from_flat(ext_adv_data_full,
                                              ext_adv_size_full);
    if (!ext_adv_mbuf || !ext_adv_full_mbuf)
    {
        ESP_LOGE("WKC_BLE", "Failed to create advertise mbuf.");
        goto set_adv_fields_end;
    }

    rc = ble_gap_ext_adv_set_data(0, ext_adv_mbuf);
    // The host takes ownership of the mbuf chain and frees it, even on error
    ext_adv_mbuf = NULL;
    if (rc)
    {
        ESP_LOGE("WKC_BLE", "Failed to set advertise data, reason: %d\n", rc);
        goto set_adv_fields_end;
    }

    // The complete manufacturer data is advertised as regular advertising
    // data, so it is delivered to passive and active scanners alike.
    rc = ble_gap_ext_adv_set_data(1, ext_adv_full_mbuf);
    ext_adv_full_mbuf = NULL;
    if (rc)
    {
        ESP_LOGE("WKC_BLE", "Failed to set full advertise data, reason: %d\n", rc);
        goto set_adv_fields_end;
    }

    set_adv_fields_end:

    if (ext_adv_data)
        free(ext_adv_data);
    if (mfg_stream)
        free(mfg_stream);
    if (ext_adv_mbuf)
        os_mbuf_free_chain(ext_adv_mbuf);

    if (ext_adv_data_full)
        free(ext_adv_data_full);
    if (mfg_arena)
        free(mfg_arena);
    if (ext_adv_full_mbuf)
        os_mbuf_free_chain(ext_adv_full_mbuf);
    return rc;
}


static void advertise()
{
    wkc_userprofile_acquire_semaphore();
    char *device_name = wkc_userprofile_get_current()->device_name;
    int rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0)
    {
        ESP_LOGE("WKC_BLE", "failed to set device name to %s, error code: %d",
                 device_name, rc);
        wkc_userprofile_release_semaphore();
        return;
    }

    if (set_adv_fields())
    {
        wkc_userprofile_release_semaphore();
        return;
    }

    rc = ble_gap_ext_adv_start(0, 0, 0);
    int rc_full = ble_gap_ext_adv_start(1, 0, 0);
    ESP_LOGI("WKC_BLE", "Start advertise.");

    if(rc == 0 && rc_full == 0)
        ui_home_update_from_shell(current_shell);
    else
    {
        if (rc)
            ESP_LOGE("WKC_BLE", "ext_adv start failed, reason: %d", rc);
        if (rc_full)
            ESP_LOGE("WKC_BLE", "full data ext_adv start failed, reason: %d", rc_full);
    }
    wkc_userprofile_release_semaphore();
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            temporary_device_append(event->connect.conn_handle);
            if(wkc_settings_get_current()->keep_advertise)
                advertise();
            else
                ui_home_update_from_shell(current_shell);
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            temporary_device_remove(event->disconnect.conn.conn_handle);
            protocol_chat_sessions_unregister(event->disconnect.conn.conn_handle);
            ESP_LOGI("WKC_BLE", "Device disconnected, conn_handle: %d", event->connect.conn_handle);
            if (event->disconnect.conn.conn_handle == current_handle)
            {
                current_handle = BLE_HS_CONN_HANDLE_NONE;
                protocol_security_set_aes(NULL);
                ui_shell_show_toast(current_shell, wkc_translations_get_string("ble_device_disconnected"), 5);
                ui_shell_show_page(current_shell, ui_shell_find_page(
                    current_shell, UI_PAGE_TYPE_HOME));
                ESP_LOGI("WKC_BLE", "Active device disconnected");
                protocol_ble_reset_client_version();
            }
            // Restart advertise
            advertise();
            ui_home_update_from_shell(current_shell);
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ui_home_update_from_shell(current_shell);
            break;
        case BLE_GAP_EVENT_PARING_COMPLETE:
            // Prevent advertise stops when executing native pairing (unused)
            if (current_handle == BLE_HS_CONN_HANDLE_NONE ||
                wkc_settings_get_current()->keep_advertise)
                advertise();
            break;
        default:
            break;
    }

    return 0;
}

void protocol_ble_notify_update(wkc_category_t category, wkc_notify_type_t type,
                                const char *name, bool bypass_legacy)
{
    if (current_handle == BLE_HS_CONN_HANDLE_NONE) return;
    bool legacy = protocol_ble_compare_versions(client_version, (uint8_t[]) { 0, 3, 0, 0 }) < 0;
    if (legacy && bypass_legacy) return;
    struct os_mbuf *notify_om = ble_hs_mbuf_att_pkt();

    uint8_t *update_request_raw = calloc((name == NULL ? 0 : strlen(name)) + 3 - legacy, 1);
    uint8_t *update_request_packed = NULL;
    if (!update_request_raw) goto notify_update_end;
    update_request_raw[0] = type;
    if (!legacy)
        update_request_raw[1] = (uint8_t)category;
    if (name != NULL)
        sprintf((char*)&update_request_raw[2 - legacy], "%s", name);
    int update_request_length;
    update_request_packed = wkc_command_pack(update_request_raw,
                            (name == NULL ? 0 : strlen(name)) + 2 - legacy,
                            &update_request_length);
    if (!update_request_packed)
        goto notify_update_end;

    os_mbuf_append(notify_om, update_request_packed, update_request_length);
    uint16_t command_attr_handle;
    ble_uuid16_t svc_uuid = BLE_UUID16_INIT(GATT_SVC_UUID);
    ble_uuid16_t chr_uuid = BLE_UUID16_INIT(GATT_CHR_COMMAND_UUID);
    ble_gatts_find_chr(&svc_uuid.u, &chr_uuid.u,
                      NULL,  &command_attr_handle);
    ble_gatts_notify_custom(current_handle, command_attr_handle, notify_om);

    notify_update_end:
    if (update_request_raw)
        free(update_request_raw);
    if (update_request_packed)
        free(update_request_packed);
    os_mbuf_free_chain(notify_om);
}

void protocol_ble_notify_update_command(wkc_category_t category, const char *name)
{
    protocol_ble_notify_update(category, WKC_NOTIFY_UPDATE_REQUIRED, name, false);
}

void protocol_ble_notify_update_table(wkc_category_t category, const char *name)
{
    protocol_ble_notify_update(category, WKC_NOTIFY_UPDATE_TABLE_REQUIRED, name, true);
}

void protocol_ble_set_client_version(const uint8_t *version)
{
    memcpy(client_version, version, 4);
}

void protocol_ble_reset_client_version()
{
    memcpy(client_version, (uint8_t[4]){ 0 }, 4);
}

int protocol_ble_compare_versions(const uint8_t *v1, const uint8_t *v2)
{
    return v1[0] > v2[0] ? 3:
           v1[0] < v2[0] ? -3:
           v1[1] > v2[1] ? 2:
           v1[1] < v2[1] ? -2:
           v1[2] > v2[2] ? 1:
           v1[2] < v2[2] ? -1:
           0;
}

int protocol_ble_compare_native_version(const uint8_t *version)
{
    return protocol_ble_compare_versions(version, protocol_version);
}

uint16_t protocol_ble_get_current_handle()
{
    return current_handle;
}

bool protocol_ble_is_device_connected()
{
    return current_handle != BLE_HS_CONN_HANDLE_NONE;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_SECURITY_UUID),
                .access_cb = gatt_svr_security_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_COMMAND_UUID),
                .access_cb = gatt_svr_command_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_CHAT_UUID),
                .access_cb = gatt_svr_chat_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
},
            { 0 }
        }
    }, { 0 }
};

static void gatt_svr_init(void)
{
    int rc;

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0)
    {
        ESP_LOGE("WKC_BLE", "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0)
    {
        ESP_LOGE("WKC_BLE", "ble_gatts_add_svcs failed: %d", rc);
        return;
    }
}

static void ble_polling_task(void *params)
{
    while (true)
    {
        int64_t current_time = esp_timer_get_time();
        for (int i = 0; i < temporary_devices_count; i++)
        {
            ble_temporary_device_t *device = &temporary_devices[i];
            if (device->tolerance == BLE_CONNECT_TIME_TOLERANCE_SHORT &&
                current_time - device->last_connect_time_us > (int)1e7 ||
                device->tolerance == BLE_CONNECT_TIME_TOLERANCE_LONG &&
                current_time - device->last_connect_time_us > (int)3e7)
                ble_gap_terminate(device->conn_handle, 0x08);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

}

static void ble_app_task(void *pvParameter)
{
    nimble_port_run();
    vTaskDelete(NULL);
}

void protocol_ble_init(ui_shell_t *shell)
{
    current_shell = shell;
    int rc;
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    if (!ble_semaphore)
        ble_semaphore = xSemaphoreCreateMutex();

    rc = nimble_port_init();
    if (rc != ESP_OK)
    {
        ESP_LOGE("WKC_BLE", "nimble_port_init failed: %d", rc);
        return;
    }
    // Init security
    protocol_security_init();
    protocol_chat_init();
    ESP_LOGI("WKC_BLE", "Protocol security okay");

    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;

    gatt_svr_init();
    nimble_port_freertos_init(ble_app_task);
    xTaskCreatePinnedToCore(ble_polling_task, "ble_polling_task", 3072, NULL, 2, NULL, 1);

    vTaskDelay(50 / portTICK_PERIOD_MS);
    advertise();
    ESP_LOGI("WKC_BLE", "Protocol BLE okay");
}

uint8_t *protocol_ble_get_version()
{
    return protocol_version;
}

uint8_t *protocol_ble_get_client_version()
{
    return client_version;
}