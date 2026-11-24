#pragma once

#include "stdbool.h"
#include "cJSON.h"
#include "ui/ui_common.h"

#define PROTOCOL_READ_REQUEST_LENGTH 200

typedef enum
{
    WKC_CMD_NONE,
    WKC_CMD_KEY_CODE,
    WKC_CMD_TIME_SYNC, // Deprecated
    WKC_CMD_READ_SHORTCUT_TABLE, // Update all if name not specified
    WKC_CMD_READ_SHORTCUT_ITEM,
    WKC_CMD_WRITE_SHORTCUT,
    WKC_CMD_READ_SETTINGS_TABLE,
    WKC_CMD_READ_SETTINGS_ITEM,
    WKC_CMD_WRITE_SETTINGS
} wkc_command_t;

typedef enum
{
    WKC_CATEGORY_SHORTCUT,
    WKC_CATEGORY_SETTINGS
} wkc_category_t;

typedef enum
{
    WKC_TABLE_ITEM_ACTION,
    WKC_TABLE_ITEM_SWITCH,
    WKC_TABLE_ITEM_INTEGER,
    WKC_TABLE_ITEM_DECIMAL,
    WKC_TABLE_ITEM_PICKER,
    WKC_TABLE_ITEM_STRING,
    WKC_TABLE_ITEM_SKIP,
    WKC_TABLE_ITEM_END
} wkc_table_item_type_t;

typedef enum
{
    WKC_NOTIFY_SUCCESSED = 0x00,
    WKC_NOTIFY_FAILED = 0x01,
    WKC_NOTIFY_CONNECTION_OCCUPIED = 0x02,
    WKC_NOTIFY_UPDATE_REQUIRED = 0x10,
    WKC_NOTIFY_UPDATE_TABLE_REQUIRED = 0x11
} wkc_notify_type_t;

typedef enum
{
    WKC_MFG_BLOCK_OWNER = 0x00,
    WKC_MFG_BLOCK_CHARACTER = 0x01,
    WKC_MFG_BLOCK_MANUFACTURER = 0x02,
    WKC_MFG_BLOCK_MODEL = 0x03,
    WKC_MFG_BLOCK_PERSONAL_SIGNATURE = 0x04,
    WKC_MFG_BLOCK_MINIMAL_SUPPORTED_VERSION = 0x05,
    WKC_MFG_BLOCK_BACKGROUND_COLOR = 0x06,
    WKC_MFG_BLOCK_BACKGROUND_IMAGE_URL = 0x07,
    WKC_MFG_BLOCK_CHAT_STATUS = 0x08,
    WKC_MFG_BLOCK_MANUFACTURER_SPECIFIC = 0xff
} wkc_mfg_block_t;

typedef enum
{
    WKC_CHAT_STATUS_DISABLED = 0x00,
    WKC_CHAT_STATUS_LIMITED = 0x01,
    WKC_CHAT_STATUS_OPEN = 0x02
} wkc_chat_status_t;

typedef struct
{
    wkc_table_item_type_t type;
    char *name;
    char *display_name;
    double min;
    double value;
    union
    {
        double max;
        int count;
        int length;
    };
    union
    {
        char *string;
        char **options;
    };
} wkc_table_item_t;

typedef struct
{
    char *name;
    char *display_name;
    wkc_table_item_t *items;
} wkc_table_group_t;

uint8_t *wkc_command_unpack(uint8_t *command, int length, int *output_length);
uint8_t *wkc_command_pack(uint8_t *command, int length, int *output_length);
int wkc_write_command(uint16_t conn_handle, uint16_t attr_handle,
uint8_t* command, int length, ui_shell_t *shell);
uint8_t *wkc_get_command_output(int *length);
cJSON *wkc_table_item_extract(wkc_table_item_t *item, bool value_only);
char *wkc_table_build(wkc_table_group_t *groups, int group_num, char *name);
wkc_table_item_t *wkc_table_group_find_item(wkc_table_group_t *groups, int count,
                                            const char *name);
bool wkc_verify_rx_timestamp(uint64_t time_data);
bool wkc_verify_conn_timestamp(uint64_t time_data);
void wkc_append_conn_timestamp(uint64_t time_data);