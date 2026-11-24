#include <stdio.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "settings.h"
#include "io/filesystem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SETTINGS_PATH_DEFAULT "/data_static/profile/settings.json"
#define SETTINGS_PATH_ACTIVE "/data_dynamic/profile/settings.json"
#define SECURITY_PATH_ACTIVE "/data_dynamic/profile/security.bin"
#define CHAT_ONLY_PATH_ACTIVE "/data_dynamic/profile/chat_only.bin"

static wkc_settings_t current_settings = { 0 };
SemaphoreHandle_t settings_semaphore = NULL;

int current_security_storage_length = 0;
int current_chat_only_storage_length = 0;
uint8_t current_security_storage[10][16] = { 0 };
uint8_t current_chat_only_storage[20][16] = { 0 };

int clamp(int value, int min, int max)
{
    return value < min ? min : value > max ? max : value;
}

void local_clamp(int *value, int min, int max)
{
    *value = *value < min ? min : *value > max ? max : *value;
}

wkc_settings_t *wkc_settings_get_current()
{
    return &current_settings;
}

void wkc_settings_load_default()
{
    wkc_copy(SETTINGS_PATH_DEFAULT, SETTINGS_PATH_ACTIVE);
}

void wkc_security_load_default()
{
    wkc_save(SECURITY_PATH_ACTIVE, (char*)current_security_storage, 0);
    wkc_save(CHAT_ONLY_PATH_ACTIVE, (char*)current_chat_only_storage, 0);
}

static void settings_parse()
{
    size_t settings_size;
    wkc_get_file_size(SETTINGS_PATH_ACTIVE, &settings_size);
    char settings_buffer[settings_size];
    wkc_open(SETTINGS_PATH_ACTIVE, settings_buffer, settings_size);

    cJSON *settings_json = cJSON_Parse(settings_buffer);
    if(settings_json == NULL)
    {
        wkc_settings_load_default();
        settings_parse();
        return;
    }
    current_settings = (wkc_settings_t) { 0 };
    cJSON *language = cJSON_GetObjectItem(settings_json, "language"),
          *keep_advertise = cJSON_GetObjectItem(settings_json, "keep_advertise"),
          *power_save = cJSON_GetObjectItem(settings_json, "power_save"),
          *display = cJSON_GetObjectItem(settings_json, "display"),
          *peripherals = cJSON_GetObjectItem(settings_json, "peripherals"),
          *homepage_status_bar_position = cJSON_GetObjectItem(settings_json, "homepage_status_bar_position"),
          *chat_status = cJSON_GetObjectItem(settings_json, "chat_status"),
          *background_color = cJSON_GetObjectItem(settings_json, "background_color");

    if (language)
        current_settings.language = (int)cJSON_GetNumberValue(language);
    if (keep_advertise)
        current_settings.keep_advertise = cJSON_IsTrue(keep_advertise);
    if (power_save)
        current_settings.power_save = clamp((int)cJSON_GetNumberValue(power_save), 0, 3);
    if (display)
    {
        cJSON *orientation = cJSON_GetObjectItem(display, "orientation"),
              *output_mode = cJSON_GetObjectItem(display, "output_mode"),
              *position = cJSON_GetObjectItem(display, "position"),
              *capture_index = cJSON_GetObjectItem(display, "capture_index"),
              *capture_osd = cJSON_GetObjectItem(display, "capture_osd");

        if (orientation)
            current_settings.display.orientation = (int)cJSON_GetNumberValue(orientation);
        if (output_mode)
            current_settings.display.output_mode = (int)cJSON_GetNumberValue(output_mode);
        if (capture_index)
            current_settings.display.capture_index = (int)cJSON_GetNumberValue(capture_index);
        if (output_mode)
            current_settings.display.capture_osd = (int)cJSON_IsTrue(capture_osd);
        if (homepage_status_bar_position)
            current_settings.homepage_status_bar_position = !!(int)cJSON_GetNumberValue(homepage_status_bar_position);
        if (chat_status)
            current_settings.chat_status = (int)cJSON_GetNumberValue(chat_status);
        if (background_color)
            current_settings.background_color = clamp((int)cJSON_GetNumberValue(
                                                background_color), -1, 0xFFFFFF);
        else
            current_settings.background_color = -1;

        if (position && cJSON_GetArraySize(position) >= 2)
        {
            for (int i = 0; i < 2; i++)
            {
                cJSON *position_item = cJSON_GetArrayItem(position, i);

                cJSON *x_offset = cJSON_GetObjectItem(position_item , "x_offset"),
                      *y_offset = cJSON_GetObjectItem(position_item , "y_offset"),
                      *output_timing = cJSON_GetObjectItem(position_item , "output_timing");

                if (x_offset)
                    current_settings.display.position[i].x_offset =
                        clamp((int)cJSON_GetNumberValue(x_offset), -128, 127);
                if (y_offset)
                    current_settings.display.position[i].y_offset =
                        clamp((int)cJSON_GetNumberValue(y_offset), -16, 16);
                if (output_timing)
                    current_settings.display.position[i].output_timing =
                        clamp((int)cJSON_GetNumberValue(output_timing), -15, 15);
            }
        }
    }
    if (peripherals)
    {
        cJSON *remember_state = cJSON_GetObjectItem(peripherals, "remember_state");
        if (remember_state && cJSON_IsTrue(remember_state))
        {
            current_settings.peripherals.remember_state = true;
            cJSON *fan = cJSON_GetObjectItem(peripherals, "fan"),
                  *eye = cJSON_GetObjectItem(peripherals, "eye");

            if (fan)
                current_settings.peripherals.fan = clamp((int)cJSON_GetNumberValue(fan), 0, 3);
            if (eye)
                current_settings.peripherals.eye = clamp((int)cJSON_GetNumberValue(eye), 0, 3);
        }
    }

    current_settings.chat_passkey = esp_random() % (int)1e06;

    cJSON_Delete(settings_json);
}

static void keys_parse(const char *path, uint8_t *storage, int *length, int storage_size)
{
    size_t keys_size;
    if (wkc_get_file_size(path, &keys_size))
    {
        *length = 0;
        return;
    }
    if (keys_size > storage_size)
        keys_size = storage_size;
    wkc_open(path, (char*)storage, keys_size);
    *length = keys_size / 16;
}

static void security_parse()
{
    keys_parse(SECURITY_PATH_ACTIVE, (uint8_t*)current_security_storage,
        &current_security_storage_length,  sizeof(current_security_storage));

    keys_parse(CHAT_ONLY_PATH_ACTIVE, (uint8_t*)current_chat_only_storage,
        &current_chat_only_storage_length,  sizeof(current_chat_only_storage));
}

static char* settings_write()
{
    cJSON *settings_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(settings_json, "language", current_settings.language);
    cJSON_AddBoolToObject(settings_json, "keep_advertise", current_settings.keep_advertise);
    cJSON_AddNumberToObject(settings_json, "power_save", current_settings.power_save);
    cJSON_AddNumberToObject(settings_json, "homepage_status_bar_position", current_settings.homepage_status_bar_position);
    cJSON_AddNumberToObject(settings_json, "chat_status", current_settings.chat_status);
    cJSON_AddNumberToObject(settings_json, "background_color", current_settings.background_color);
    cJSON *display = cJSON_AddObjectToObject(settings_json, "display");
    cJSON_AddNumberToObject(display, "orientation", current_settings.display.orientation);
    cJSON_AddNumberToObject(display, "output_mode", current_settings.display.output_mode);
    cJSON *position = cJSON_AddArrayToObject(display, "position");
    for (int i = 0; i < 2; i++)
    {
        cJSON *position_item = cJSON_CreateObject();
        cJSON_AddNumberToObject(position_item, "x_offset",
            current_settings.display.position[i].x_offset);
        cJSON_AddNumberToObject(position_item, "y_offset",
            current_settings.display.position[i].y_offset);
        cJSON_AddNumberToObject(position_item, "output_timing",
            current_settings.display.position[i].output_timing);
        cJSON_AddItemToArray(position, position_item);
    }
    cJSON_AddNumberToObject(display, "capture_index", current_settings.display.capture_index);
    cJSON_AddBoolToObject(display, "capture_osd", current_settings.display.capture_osd);
    cJSON *peripherals = cJSON_AddObjectToObject(settings_json, "peripherals");
    cJSON_AddBoolToObject(peripherals, "remember_state",
        current_settings.peripherals.remember_state);
    cJSON_AddNumberToObject(peripherals, "fan", current_settings.peripherals.fan);
    cJSON_AddNumberToObject(peripherals, "eye", current_settings.peripherals.eye);
    char *result = cJSON_Print(settings_json);
    cJSON_Delete(settings_json);
    return result;
}

void wkc_settings_save()
{
    xSemaphoreTake(settings_semaphore, portMAX_DELAY);
    char *result = settings_write();
    wkc_save(SETTINGS_PATH_ACTIVE, result, strlen(result) + 1);
    free(result);
    xSemaphoreGive(settings_semaphore);
}

static void wkc_security_save()
{
    wkc_save(SECURITY_PATH_ACTIVE, (char*)current_security_storage,
             16 * current_security_storage_length);
    wkc_save(CHAT_ONLY_PATH_ACTIVE, (char*)current_chat_only_storage,
             16 * current_chat_only_storage_length);
}

static void append_key(uint8_t *key, uint8_t *storage, int *length, int max_length)
{
    xSemaphoreTake(settings_semaphore, portMAX_DELAY);
    for (int i = max_length - 2; i >= 0; i--)
    {
        memcpy(&storage[(i + 1) * 16], &storage[i * 16], 16);
    }
    memcpy(&storage[0], key, 16);
    if (*length < max_length) *length += 1;
    wkc_security_save();
    xSemaphoreGive(settings_semaphore);
}

void wkc_security_append_key(uint8_t *key)
{
    append_key(key, (uint8_t*)current_security_storage, &current_security_storage_length,
        sizeof(current_security_storage) / 16);
}

void wkc_chat_only_append_key(uint8_t *key)
{
    append_key(key, (uint8_t*)current_chat_only_storage, &current_chat_only_storage_length,
        sizeof(current_chat_only_storage) / 16);
}

void wkc_settings_init()
{
    if (settings_semaphore == NULL)
        settings_semaphore = xSemaphoreCreateMutex();
    if(!wkc_file_exist(SETTINGS_PATH_ACTIVE))
    {
        ESP_LOGW("WKC_SETTINGS", "Settings does not exist, create default");
        wkc_settings_load_default();
        wkc_settings_init();
        return;
    }
    settings_parse();
}

void wkc_security_init()
{
    if(!wkc_file_exist(SECURITY_PATH_ACTIVE))
    {
        ESP_LOGW("WKC_SETTINGS", "Security does not exist, create default");
        wkc_security_load_default();
        wkc_security_init();
        return;
    }
    security_parse();
}

int wkc_get_security_storage_length()
{
    return current_security_storage_length;
}

int wkc_get_chat_only_storage_length()
{
    return current_chat_only_storage_length;
}

uint8_t *wkc_security_storage_get_current()
{
    return (uint8_t*)current_security_storage;
}

uint8_t *wkc_chat_only_storage_get_current()
{
    return (uint8_t*)current_chat_only_storage;
}