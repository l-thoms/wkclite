#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "userprofile.h"
#include "cJSON.h"
#include "io/filesystem.h"

#define TAG "WKC_USERPROFILE"

#define USERPROFILE_PATH_DEFAULT "/data_static/profile/userprofile.json"
#define USERPROFILE_PATH_ACTIVE "/data_dynamic/profile/userprofile.json"
// Sanity limit for the profile file; anything larger is treated as corrupt
#define USERPROFILE_FILE_MAX 8192

wkc_userprofile_t current_profile;
static SemaphoreHandle_t userprofile_semaphore;

void wkc_userprofile_load_default()
{
    if (!wkc_file_exist(USERPROFILE_PATH_DEFAULT))
    {
        ESP_LOGE(TAG, "Default userprofile is missing");
        return;
    }
    if (wkc_copy(USERPROFILE_PATH_DEFAULT, USERPROFILE_PATH_ACTIVE) != 0)
        ESP_LOGE(TAG, "Failed to copy the default userprofile");
}

// Reads the whole userprofile file into a NUL terminated heap buffer
static char *userprofile_read_file()
{
    size_t profile_size = 0;
    if (wkc_get_file_size(USERPROFILE_PATH_ACTIVE, &profile_size) != 0 ||
        profile_size == 0 || profile_size > USERPROFILE_FILE_MAX)
        return NULL;

    char *buffer = malloc(profile_size + 1);
    if (buffer == NULL)
        return NULL;

    int read = wkc_read(USERPROFILE_PATH_ACTIVE, buffer, profile_size);
    if (read < 0)
    {
        free(buffer);
        return NULL;
    }
    buffer[read] = '\0';
    return buffer;
}

static cJSON *userprofile_read_json()
{
    char *buffer = userprofile_read_file();
    if (buffer == NULL)
        return NULL;
    cJSON *profile_json = cJSON_Parse(buffer);
    free(buffer);
    return profile_json;
}

// Keeps the unreadable file for diagnostics, then restores the factory default
static void userprofile_restore_default()
{
    char backup_path[64];
    if (snprintf(backup_path, sizeof(backup_path), "%s.bad", USERPROFILE_PATH_ACTIVE)
        < (int)sizeof(backup_path))
    {
        remove(backup_path);
        if (rename(USERPROFILE_PATH_ACTIVE, backup_path) != 0)
            ESP_LOGW(TAG, "Failed to keep the broken userprofile file");
    }
    wkc_userprofile_load_default();
}

static void userprofile_parse()
{
    cJSON *profile_json = userprofile_read_json();
    if (profile_json == NULL)
    {
        ESP_LOGE(TAG, "Userprofile unreadable, restoring defaults");
        userprofile_restore_default();
        profile_json = userprofile_read_json();
    }
    if (profile_json == NULL)
    {
        ESP_LOGE(TAG, "Default userprofile unreadable, keeping built-in values");
        return;
    }
    cJSON *device_name = cJSON_GetObjectItem(profile_json, "device_name"),
          *owner = cJSON_GetObjectItem(profile_json, "owner"),
          *character = cJSON_GetObjectItem(profile_json, "character"),
          *manufacturer = cJSON_GetObjectItem(profile_json, "manufacturer"),
          *personal_signature = cJSON_GetObjectItem(profile_json, "personal_signature"),
          *background_image_url = cJSON_GetObjectItem(profile_json, "background_image_url");

    char *device_name_str = cJSON_GetStringValue(device_name),
         *owner_str = cJSON_GetStringValue(owner),
         *character_str = cJSON_GetStringValue(character),
         *manufacturer_str = cJSON_GetStringValue(manufacturer),
         *personal_signature_str = cJSON_GetStringValue(personal_signature),
         *background_image_url_str = cJSON_GetStringValue(background_image_url);

    current_profile.device_name = device_name_str ? strdup(device_name_str) : NULL;
    current_profile.owner = owner_str ? strdup(owner_str) : NULL;
    current_profile.character = character_str ? strdup(character_str) : NULL;
    current_profile.manufacturer = manufacturer_str ? strdup(manufacturer_str) : NULL;
    current_profile.personal_signature = personal_signature_str ? strdup(personal_signature_str) : NULL;
    current_profile.background_image_url = background_image_url_str ? strdup(background_image_url_str) : NULL;

    cJSON_Delete(profile_json);
}

void wkc_userprofile_save()
{
    // Recursive: the protocol handlers already own the semaphore while they
    // update the profile and then save it, all from the same task
    xSemaphoreTakeRecursive(userprofile_semaphore, portMAX_DELAY);
    cJSON *profile_json = cJSON_CreateObject();
    if (profile_json != NULL)
    {
        cJSON_AddStringToObject(profile_json, "device_name",
                                current_profile.device_name ? current_profile.device_name : "");
        cJSON_AddStringToObject(profile_json, "owner",
                                current_profile.owner ? current_profile.owner : "");
        cJSON_AddStringToObject(profile_json, "character",
                                current_profile.character ? current_profile.character : "");
        cJSON_AddStringToObject(profile_json, "manufacturer",
                                current_profile.manufacturer ? current_profile.manufacturer : "");
        cJSON_AddStringToObject(profile_json, "personal_signature",
                                current_profile.personal_signature ?
                                current_profile.personal_signature : "");
        cJSON_AddStringToObject(profile_json, "background_image_url",
                                current_profile.background_image_url ?
                                current_profile.background_image_url : "");
        char *result = cJSON_Print(profile_json);
        cJSON_Delete(profile_json);
        if (result != NULL)
        {
            if (wkc_save(USERPROFILE_PATH_ACTIVE, result, strlen(result) + 1) != 0)
                ESP_LOGE(TAG, "Failed to save the userprofile");
            free(result);
        }
        else
            ESP_LOGE(TAG, "Failed to serialize the userprofile");
    }
    xSemaphoreGiveRecursive(userprofile_semaphore);
}

void wkc_userprofile_init()
{
    if (!userprofile_semaphore)
        userprofile_semaphore = xSemaphoreCreateRecursiveMutex();
    if(!wkc_file_exist(USERPROFILE_PATH_ACTIVE))
    {
        ESP_LOGW(TAG, "Userprofile does not exist, create default");
        wkc_userprofile_load_default();
    }
    userprofile_parse();
}

wkc_userprofile_t *wkc_userprofile_get_current()
{
    return &current_profile;
}

void wkc_userprofile_acquire_semaphore()
{
    if (userprofile_semaphore != NULL)
        xSemaphoreTakeRecursive(userprofile_semaphore, portMAX_DELAY);
}

void wkc_userprofile_release_semaphore()
{
    if (userprofile_semaphore != NULL)
        xSemaphoreGiveRecursive(userprofile_semaphore);
}
