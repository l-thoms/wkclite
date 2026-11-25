#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_random.h"
#include "protocol/ble.h"
#include "protocol/command_parser.h"
#include "display/graphics.h"
#include "display/display_control.h"
#include "ui_common.h"
#include "camera/camera_control.h"
#include "shell.h"
#include "math.h"
#include "profile/translations.h"
#include "profile/settings.h"
#include "profile/userprofile.h"
#include "peripherals/battery_calibration.h"

#define UI_SETTINGS_MENU_MAX_DEPTH 8

typedef enum
{
    UI_SETTINGS_SPECIAL_NONE,
    UI_SETTINGS_SPECIAL_POSITION,
    UI_SETTINGS_SPECIAL_CONFIRM
} ui_settings_special_t;

typedef struct
{
    ui_page_t base;
    bool show;
    bool draw_request;
    // Menu stack, the top most menu is the one currently shown
    ui_menu_item_t *current_menu[UI_SETTINGS_MENU_MAX_DEPTH];
    int selected_index[UI_SETTINGS_MENU_MAX_DEPTH];
    int last_selected_index;
    // Full screen modes that are not list menus
    ui_settings_special_t special;
    int special_index;
    // Which entry opened the confirmation dialog (1 devices, 2 factory reset)
    int confirm_target;
    // Tiled output: the first screen shows the parent menu. It only has to be
    // repainted after an orientation/format change, not when a sub menu is
    // opened from an intact parent screen.
    bool parent_needs_repaint;
} ui_settings_t;

static ui_menu_item_t *ui_settings_build_main_menu();
static ui_menu_item_t *ui_settings_build_display_menu();
static ui_menu_item_t *ui_settings_build_misc_menu();
static ui_menu_item_t *ui_settings_build_advanced_menu();

#pragma region menu stack

static void ui_settings_menu_set_options(ui_menu_item_t *item, char **options, int count)
{
    item->count = count;
    if (count <= 0)
    {
        item->options = NULL;
        return;
    }
    item->options = calloc(count, sizeof(char*));
    for (int i = 0; i < count; i++)
        item->options[i] = strdup(options[i]);
}

static int ui_settings_get_menu_count(ui_settings_t *settings)
{
    int count = 0;
    while (count < UI_SETTINGS_MENU_MAX_DEPTH && settings->current_menu[count] != NULL)
        count += 1;
    return count;
}

static void ui_settings_menu_free(ui_menu_item_t *menu)
{
    if (menu == NULL) return;
    int index = 0;
    while (menu[index].type != UI_MENU_ITEM_END)
    {
        if (menu[index].options != NULL)
        {
            for (int i = 0; i < menu[index].count; i++)
                if (menu[index].options[i] != NULL) free(menu[index].options[i]);
            free(menu[index].options);
        }
        index += 1;
    }
    free(menu);
}

static void ui_settings_menu_push(ui_settings_t *settings, ui_menu_item_t *menu)
{
    if (menu == NULL) return;
    int count = ui_settings_get_menu_count(settings);
    if (count >= UI_SETTINGS_MENU_MAX_DEPTH)
    {
        ui_settings_menu_free(menu);
        return;
    }
    settings->current_menu[count] = menu;
    settings->selected_index[count] = 0;
    settings->last_selected_index = -1;
    settings->parent_needs_repaint = false;
    settings->show = true;
    settings->draw_request = true;
}

static bool ui_settings_menu_pop(ui_settings_t *settings)
{
    int count = ui_settings_get_menu_count(settings);
    if (count <= 0) return false;
    ui_settings_menu_free(settings->current_menu[count - 1]);
    settings->current_menu[count - 1] = NULL;
    settings->last_selected_index = -1;
    settings->parent_needs_repaint = true;
    settings->show = true;
    settings->draw_request = true;
    return count > 1;
}

static void ui_settings_menu_free_all(ui_settings_t *settings)
{
    int count = ui_settings_get_menu_count(settings);
    while (count > 0)
    {
        ui_settings_menu_free(settings->current_menu[count - 1]);
        settings->current_menu[count - 1] = NULL;
        count -= 1;
    }
}

#pragma endregion

#pragma region menu actions

// Opens a sub menu, mirroring the camera's sub menu entries
static void ui_settings_open_display_action(ui_menu_item_t *item, ui_page_t *page)
{
    ui_settings_menu_push((ui_settings_t*)page, ui_settings_build_display_menu());
}

static void ui_settings_open_misc_action(ui_menu_item_t *item, ui_page_t *page)
{
    ui_settings_menu_push((ui_settings_t*)page, ui_settings_build_misc_menu());
}

static void ui_settings_open_advanced_action(ui_menu_item_t *item, ui_page_t *page)
{
    ui_settings_menu_push((ui_settings_t*)page, ui_settings_build_advanced_menu());
}

static void ui_settings_language_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->language = item->current_value;
    wkc_settings_save();
    // Every label comes from the loaded translations, so refresh the whole UI
    ui_shell_refresh(ui_shell_get_current());
}

static void ui_settings_orientation_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->display.orientation = item->current_value;
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "screen_orientation");
}

static void ui_settings_output_mode_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->display.output_mode = item->current_value;
    display_settings_update();
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "output_mode");
}

static void ui_settings_adjust_position_action(ui_menu_item_t *item, ui_page_t *page)
{
    ui_settings_t *settings = (ui_settings_t*)page;
    settings->special = UI_SETTINGS_SPECIAL_POSITION;
    settings->special_index = 0;
    settings->draw_request = true;
    settings->show = true;
}

static void ui_settings_capture_index_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->display.capture_index = item->current_value;
    display_settings_update();
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "capture_card_output");
}

static void ui_settings_capture_osd_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->display.capture_osd = item->current_value;
    display_settings_update();
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "capture_with_osd");
}

static void ui_settings_power_save_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->power_save = item->current_value;
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "power_save");
}

static void ui_settings_keep_advertise_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->keep_advertise = item->current_value;
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "keep_advertise");
}

static void ui_settings_remember_peripherals_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->peripherals.remember_state = item->current_value;
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "remember_peripherals");
}

static void ui_settings_status_bar_position_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->homepage_status_bar_position = item->current_value;
    wkc_settings_save();
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS,
        "homepage_status_bar_position");
}

// Chat state is part of the misc settings table reported to the client
static void ui_settings_chat_status_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_get_current()->chat_status = (wkc_chat_status_t)item->current_value;
    wkc_settings_save();
    protocol_ble_notify_update_table(WKC_CATEGORY_SETTINGS, "misc_settings");
}

// The chat passkey has no values to cycle through: left/right regenerate it
static void ui_settings_chat_passkey_action(ui_menu_item_t *item, ui_page_t *page)
{
    wkc_settings_t *settings = wkc_settings_get_current();
    settings->chat_passkey = esp_random() % (int)1e06;
    if (item->options != NULL && item->count > 0)
    {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%06d", settings->chat_passkey);
        free(item->options[0]);
        item->options[0] = strdup(buffer);
    }
    protocol_ble_notify_update_command(WKC_CATEGORY_SETTINGS, "chat_passkey");
}

static void ui_settings_calibration_action(ui_menu_item_t *item, ui_page_t *page)
{
    battery_calibration_set(!battery_calibration_is_calibrating());
    item->current_value = battery_calibration_is_calibrating();
}

static void ui_settings_reset_confirm_action(ui_menu_item_t *item, ui_page_t *page)
{
    ui_settings_t *settings = (ui_settings_t*)page;
    int count = ui_settings_get_menu_count(settings);
    int selected = count > 0 ? settings->selected_index[count - 1] : 0;
    settings->confirm_target = selected == 1 ? 1 : 2;
    settings->special = UI_SETTINGS_SPECIAL_CONFIRM;
    settings->special_index = 0;
    // Only the dialog is drawn: the menu below it is still intact. A full
    // repaint is only requested by ui_settings_on_refresh (phone side change,
    // orientation change).
    settings->draw_request = true;
}

#pragma endregion

#pragma region menu builders

static ui_menu_item_t *ui_settings_build_main_menu()
{
    ui_menu_item_t *menu = calloc(5, sizeof(ui_menu_item_t));
    menu[0] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_LABEL,
        .name = wkc_translations_get_string("settings_display_settings"),
        .action = ui_settings_open_display_action
    };
    menu[1] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_LABEL,
        .name = wkc_translations_get_string("settings_misc_settings"),
        .action = ui_settings_open_misc_action
    };
    menu[2] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_LABEL,
        .name = wkc_translations_get_string("settings_advanced_settings"),
        .action = ui_settings_open_advanced_action
    };
    int language_count = wkc_translations_get_languages_count();
    char **language_options = calloc(language_count, sizeof(char*));
    for (int i = 0; i < language_count; i++)
        language_options[i] = wkc_translations_get_language_name_from_index(i);
    menu[3] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_language"),
        .current_value = wkc_settings_get_current()->language,
        .action = ui_settings_language_action
    };
    ui_settings_menu_set_options(&menu[3], language_options, language_count);
    free(language_options);
    menu[4] = (ui_menu_item_t) { .type = UI_MENU_ITEM_END };
    return menu;
}

static ui_menu_item_t *ui_settings_build_display_menu()
{
    char *orientation_options[] = {
        wkc_translations_get_string("settings_horizontal"),
        wkc_translations_get_string("settings_vertical"),
        wkc_translations_get_string("settings_vertical_tiled")
    };
    char *output_mode_options[] = {
        wkc_translations_get_string("settings_rising_edge"),
        wkc_translations_get_string("settings_falling_edge"),
        wkc_translations_get_string("settings_debug_mode")
    };
    char *capture_index_options[] = {
        wkc_translations_get_string("settings_screen_id_1"),
        wkc_translations_get_string("settings_screen_id_2"),
    };
    ui_menu_item_t *menu = calloc(6, sizeof(ui_menu_item_t));
    menu[0] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_screen_orientation"),
        .current_value = wkc_settings_get_current()->display.orientation,
        .action = ui_settings_orientation_action
    };
    ui_settings_menu_set_options(&menu[0], orientation_options, 3);
    menu[1] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_output_mode"),
        .current_value = wkc_settings_get_current()->display.output_mode,
        .action = ui_settings_output_mode_action
    };
    ui_settings_menu_set_options(&menu[1], output_mode_options, 3);
    menu[2] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_LABEL,
        .name = wkc_translations_get_string("settings_adjust_position"),
        .action = ui_settings_adjust_position_action
    };
    menu[3] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_capture_output"),
        .current_value = wkc_settings_get_current()->display.capture_index,
        .action = ui_settings_capture_index_action
    };
    ui_settings_menu_set_options(&menu[3], capture_index_options, 2);
    menu[4] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_SWITCH,
        .name = wkc_translations_get_string("settings_capture_osd"),
        .current_value = wkc_settings_get_current()->display.capture_osd,
        .action = ui_settings_capture_osd_action
    };
    menu[5] = (ui_menu_item_t) { .type = UI_MENU_ITEM_END };
    return menu;
}

static ui_menu_item_t *ui_settings_build_misc_menu()
{
    char *power_save_options[] = {
        wkc_translations_get_string("settings_never"),
        wkc_translations_get_string("settings_1min"),
        wkc_translations_get_string("settings_5min"),
        wkc_translations_get_string("settings_10min"),
    };
    char *status_bar_position_options[] = {
        wkc_translations_get_string("settings_top"),
        wkc_translations_get_string("settings_bottom")
    };
    char *chat_status_options[] = {
        wkc_translations_get_string("settings_chat_status_disabled"),
        wkc_translations_get_string("settings_chat_status_limited"),
        wkc_translations_get_string("settings_chat_status_open")
    };
    wkc_settings_t *current_settings = wkc_settings_get_current();
    char chat_code_buffer[16];
    snprintf(chat_code_buffer, sizeof(chat_code_buffer), "%06d",
             current_settings->chat_passkey);
    char *chat_code_options[] = { chat_code_buffer };

    ui_menu_item_t *menu = calloc(7, sizeof(ui_menu_item_t));
    menu[0] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_power_save"),
        .current_value = current_settings->power_save,
        .action = ui_settings_power_save_action
    };
    ui_settings_menu_set_options(&menu[0], power_save_options, 4);
    menu[1] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_SWITCH,
        .name = wkc_translations_get_string("settings_keep_advertise"),
        .current_value = current_settings->keep_advertise,
        .action = ui_settings_keep_advertise_action
    };
    menu[2] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_SWITCH,
        .name = wkc_translations_get_string("settings_remember_peripherals"),
        .current_value = current_settings->peripherals.remember_state,
        .action = ui_settings_remember_peripherals_action
    };
    menu[3] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_homepage_status_bar_position"),
        .current_value = current_settings->homepage_status_bar_position,
        .action = ui_settings_status_bar_position_action
    };
    ui_settings_menu_set_options(&menu[3], status_bar_position_options, 2);
    menu[4] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_chat_status"),
        .current_value = (int)current_settings->chat_status,
        .action = ui_settings_chat_status_action
    };
    ui_settings_menu_set_options(&menu[4], chat_status_options, 3);
    menu[5] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_PICKER,
        .name = wkc_translations_get_string("settings_chat_code"),
        .current_value = 0,
        .action = ui_settings_chat_passkey_action
    };
    ui_settings_menu_set_options(&menu[5], chat_code_options, 1);
    menu[6] = (ui_menu_item_t) { .type = UI_MENU_ITEM_END };
    return menu;
}

static ui_menu_item_t *ui_settings_build_advanced_menu()
{
    ui_menu_item_t *menu = calloc(4, sizeof(ui_menu_item_t));
    menu[0] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_SWITCH,
        .name = wkc_translations_get_string("settings_battery_calibration_mode"),
        .current_value = battery_calibration_is_calibrating(),
        .action = ui_settings_calibration_action
    };
    menu[1] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_LABEL,
        .name = wkc_translations_get_string("settings_clear_paired_devices"),
        .action = ui_settings_reset_confirm_action
    };
    menu[2] = (ui_menu_item_t) {
        .type = UI_MENU_ITEM_LABEL,
        .name = wkc_translations_get_string("settings_factory_reset"),
        .action = ui_settings_reset_confirm_action
    };
    menu[3] = (ui_menu_item_t) { .type = UI_MENU_ITEM_END };
    return menu;
}

#pragma endregion

static void ui_settings_draw_display_position(int index, display_format_t formats[],
                                              display_orientation_t orientation,
                                              int output_timing, bool force_frame)
{
    char tip_message_upper[100];
    char tip_message_lower[100];
    int logical_width = orientation == DISPLAY_ORIENTATION_HORIZONTAL ? DISPLAY_WIDTH_PAL :
                        DISPLAY_HEIGHT_PAL;
    int logical_height = orientation == DISPLAY_ORIENTATION_HORIZONTAL ? DISPLAY_HEIGHT_PAL :
                         DISPLAY_WIDTH_PAL;
    switch (index)
    {
        case 0:
        {
            strcpy(tip_message_upper,
                wkc_translations_get_string("settings_adjusting_screen1_position"));
            strcpy(tip_message_lower,
                wkc_translations_get_string("settings_adjusting_screen1_position_next"));
        }
        break;
        case 1:
        {
            strcpy(tip_message_upper,
                wkc_translations_get_string("settings_adjusting_screen1_timing"));
            strcpy(tip_message_lower,
                wkc_translations_get_string("settings_adjusting_screen1_timing_next"));
        }
        break;
        case 2:
        {
            strcpy(tip_message_upper,
                wkc_translations_get_string("settings_adjusting_screen2_position"));
            strcpy(tip_message_lower,
                wkc_translations_get_string("settings_adjusting_screen2_position_next"));
        }
        break;
        default:
        {
            strcpy(tip_message_upper,
                wkc_translations_get_string("settings_adjusting_screen2_timing"));
            strcpy(tip_message_lower,
                wkc_translations_get_string("settings_save_and_exit"));
        }
        break;
    }

    if (index == 0 || force_frame)
    {
        DISPLAY_CLEAR_SCREEN(0);
        DISPLAY_CLEAR_SCREEN(1);

        // Draw border
        display_rect_t frame_rect = {
            .x = 16, .y = 16, .width = logical_width - 32, .height = logical_height - 32
        };
        display_rect_t frame_rect_primary = display_rect_compensation(&frame_rect, formats[0]);
        display_rect_t frame_rect_secondary = display_rect_compensation(&frame_rect, formats[1]);
        display_rect_translate(&frame_rect_primary, 1, 1);
        display_rect_translate(&frame_rect_secondary, 1, 1);
        display_draw_rect(0, orientation, &frame_rect_primary, DISPLAY_COLOR_BLACK, 1);
        display_draw_rect(1, orientation, &frame_rect_secondary, DISPLAY_COLOR_BLACK, 1);
        display_rect_translate(&frame_rect_primary, -1, -1);
        display_rect_translate(&frame_rect_secondary, -1, -1);
        display_draw_rect(0, orientation, &frame_rect_primary, DISPLAY_COLOR_WHITE, 1);
        display_draw_rect(1, orientation, &frame_rect_secondary, DISPLAY_COLOR_WHITE, 1);

        // Draw four arrows
        int arrow_size;
        display_get_icon("arrow_drop_up", DISPLAY_FORMAT_PAL, false, NULL, &arrow_size);
        for (int i = 0; i < 4; i++)
        {
            int arrow_size_primary, arrow_size_secondary;
            char arrow_name[20] = { 0 };
            char arrow_primary[50] = { 0 }, arrow_secondary[50] = { 0 };
            sprintf(arrow_name, "arrow_drop_%s", i == 0 ? "up" : i == 1 ? "right" :
                    i == 2 ? "down": "left");
            display_get_icon(arrow_name, DISPLAY_FORMAT_PAL, false, NULL, NULL);
            display_get_icon(arrow_name, formats[0], false, arrow_primary, &arrow_size_primary);
            display_get_icon(arrow_name, formats[1], false, arrow_secondary, &arrow_size_secondary);
            display_vector_t icon_position = {
                .x = i % 2 == 0 ? (logical_width - arrow_size) / 2 : i == 1 ?
                logical_width - 16 - arrow_size : 16,
                .y = i % 2 == 1 ? (logical_height - arrow_size) / 2 : i == 0 ?
                16 : logical_height - 16 - arrow_size
            };
            display_vector_t icon_position_primary = display_vector_compensation(
                &icon_position, formats[0]
            );
            display_vector_t icon_position_secondary = display_vector_compensation(
                &icon_position, formats[1]
            );
            display_vector_translate(&icon_position_primary, 1, 1);
            display_vector_translate(&icon_position_secondary, 1, 1);
            display_draw_image(0, orientation, icon_position_primary.x, icon_position_primary.y,
                               arrow_primary, arrow_size_primary, DISPLAY_COLOR_BLACK);
            display_draw_image(1, orientation, icon_position_secondary.x, icon_position_secondary.y,
                               arrow_secondary, arrow_size_secondary, DISPLAY_COLOR_BLACK);
            display_vector_translate(&icon_position_primary, -1, -1);
            display_vector_translate(&icon_position_secondary, -1, -1);
            display_draw_image(0, orientation, icon_position_primary.x, icon_position_primary.y,
                               arrow_primary, arrow_size_primary, DISPLAY_COLOR_WHITE);
            display_draw_image(1, orientation, icon_position_secondary.x, icon_position_secondary.y,
                               arrow_secondary, arrow_size_secondary, DISPLAY_COLOR_WHITE);
        }

        // Draw crosshair
        int cross_size, cross_size_primary, cross_size_secondary;
        char cross_primary[50] = { 0 }, cross_secondary[50] = { 0 };
        display_get_icon("cross", DISPLAY_FORMAT_PAL, false, NULL, &cross_size);
        display_get_icon("cross", formats[0], false, cross_primary, &cross_size_primary);
        display_get_icon("cross", formats[1], false, cross_secondary, &cross_size_secondary);
        display_vector_t cross_position = {
            .x = (logical_width - cross_size) / 2, .y = (logical_height - cross_size) / 2
        };
        display_vector_t cross_position_primary = display_vector_compensation(
            &cross_position, formats[0]
        );
        display_vector_t cross_position_secondary = display_vector_compensation(
            &cross_position, formats[1]
        );
        display_vector_translate(&cross_position_primary, 1, 1);
        display_vector_translate(&cross_position_secondary, 1, 1);
        display_draw_image(0, orientation, cross_position_primary.x, cross_position_primary.y,
                           cross_primary, cross_size_primary, DISPLAY_COLOR_BLACK);
        display_draw_image(1, orientation, cross_position_secondary.x, cross_position_secondary.y,
                           cross_secondary, cross_size_secondary, DISPLAY_COLOR_BLACK);
        display_vector_translate(&cross_position_primary, -1, -1);
        display_vector_translate(&cross_position_secondary, -1, -1);
        display_draw_image(0, orientation, cross_position_primary.x, cross_position_primary.y,
                           cross_primary, cross_size_primary, DISPLAY_COLOR_WHITE);
        display_draw_image(1, orientation, cross_position_secondary.x, cross_position_secondary.y,
                           cross_secondary, cross_size_secondary, DISPLAY_COLOR_WHITE);

        display_rect_expand(&frame_rect_primary, 2, 2);
        display_rect_expand(&frame_rect_secondary, 2, 2);
        display_update(0, orientation, &frame_rect_primary);
        display_update(1, orientation, &frame_rect_secondary);
    }

    // Clear bottom
    display_rect_t bottom_rect = {
        .x = 40, .y = logical_height / 2 + 12,
        .width = logical_width - 80, .height = logical_height / 2 - 12 - 40
    };
    display_rect_t bottom_rect_primary = display_rect_compensation(&bottom_rect, formats[0]);
    display_rect_t bottom_rect_secondary = display_rect_compensation(&bottom_rect, formats[1]);
    display_fill_rect(0, orientation, &bottom_rect_primary, DISPLAY_COLOR_TRANSPARENT);
    display_fill_rect(1, orientation, &bottom_rect_secondary, DISPLAY_COLOR_TRANSPARENT);
    // Draw text
    int item_position_y = logical_height / 2 + 24, text_length;
    display_vector_t text_size;
    for (int i = 0; i < 2; i++)
    {
        char *selected_text = i == 0 ? tip_message_upper : tip_message_lower;
        free(font_measure_text(selected_text, DISPLAY_FORMAT_PAL, 0, &text_length,
                               &text_size));
        text_position_descriptor_t *measure_result_primary = font_measure_text(selected_text,
                                                             formats[0], 0, NULL, NULL);
        text_position_descriptor_t *measure_result_secondary = font_measure_text(selected_text,
                                                               formats[1], 0, NULL, NULL);
        display_vector_t text_position_raw = { .x = (logical_width - text_size.x) / 2,
                                               .y = item_position_y };
        display_vector_t text_position_primary = display_vector_compensation(
            &text_position_raw, formats[0]
        );
        display_vector_t text_position_secondary = display_vector_compensation(
            &text_position_raw, formats[1]
        );
        display_vector_translate(&text_position_primary, 1, 1);
        display_vector_translate(&text_position_secondary, 1, 1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index < 2)
            display_draw_text(0, orientation, text_position_primary.x, text_position_primary.y,
                measure_result_primary, text_length, DISPLAY_COLOR_BLACK, formats[0]);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index >= 2)
            display_draw_text(1, orientation, text_position_secondary.x, text_position_secondary.y,
                measure_result_secondary, text_length, DISPLAY_COLOR_BLACK, formats[1]);
        display_vector_translate(&text_position_primary, -1, -1);
        display_vector_translate(&text_position_secondary, -1, -1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index < 2)
            display_draw_text(0, orientation, text_position_primary.x, text_position_primary.y,
                measure_result_primary, text_length, DISPLAY_COLOR_WHITE, formats[0]);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index >= 2)
            display_draw_text(1, orientation, text_position_secondary.x, text_position_secondary.y,
                measure_result_secondary, text_length, DISPLAY_COLOR_WHITE, formats[1]);
        item_position_y += text_size.y;
        free(measure_result_primary);
        free(measure_result_secondary);
    }

    // Draw output timing indicator
    if (index % 2 == 1)
    {
        display_vector_t indicator_position = {
            .x = (logical_width - 150) / 2, .y = item_position_y
        };
        display_rect_t indicator_rect = {
            .x = indicator_position.x, .y = indicator_position.y + 6,
            .width = 150, .height = 6
        };
        display_rect_t indicator_rect_primary = display_rect_compensation(&indicator_rect,
                                                formats[0]);
        display_rect_t indicator_rect_secondary = display_rect_compensation(&indicator_rect,
                                                  formats[1]);
        display_rect_t indicator_value_primary = indicator_rect_primary;
        display_rect_t indicator_value_secondary = indicator_rect_secondary;
        indicator_value_primary.width = (int)((float)indicator_value_primary.width *
                                        (output_timing + 15) / 30 + 0.5f);
        indicator_value_secondary.width = (int)((float)indicator_value_secondary.width *
                                          (output_timing + 15) / 30 + 0.5f);

        display_rect_translate(&indicator_rect_primary, 1, 1);
        display_rect_translate(&indicator_rect_secondary, 1, 1);
        display_rect_translate(&indicator_value_primary, 1, 1);
        display_rect_translate(&indicator_value_secondary, 1, 1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index < 2)
        {
            display_draw_rect(0, orientation, &indicator_rect_primary, DISPLAY_COLOR_BLACK, 1);
            display_fill_rect(0, orientation, &indicator_value_primary, DISPLAY_COLOR_BLACK);
        }
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index >= 2)
        {
            display_draw_rect(1, orientation, &indicator_rect_secondary, DISPLAY_COLOR_BLACK, 1);
            display_fill_rect(1, orientation, &indicator_value_secondary, DISPLAY_COLOR_BLACK);
        }
        display_rect_translate(&indicator_rect_primary, -1, -1);
        display_rect_translate(&indicator_rect_secondary, -1, -1);
        display_rect_translate(&indicator_value_primary, -1, -1);
        display_rect_translate(&indicator_value_secondary, -1, -1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index < 2)
        {
            display_draw_rect(0, orientation, &indicator_rect_primary, DISPLAY_COLOR_WHITE, 1);
            display_fill_rect(0, orientation, &indicator_value_primary, DISPLAY_COLOR_WHITE);
        }
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index >= 2)
        {
            display_draw_rect(1, orientation, &indicator_rect_secondary, DISPLAY_COLOR_WHITE, 1);
            display_fill_rect(1, orientation, &indicator_value_secondary, DISPLAY_COLOR_WHITE);
        }

        indicator_position.x += 156;
        char indicator_text[10];
        sprintf(indicator_text, "%d", output_timing);
        display_vector_t indicator_position_primary = display_vector_compensation(
            &indicator_position, formats[0]
        );
        display_vector_t indicator_position_secondary = display_vector_compensation(
            &indicator_position, formats[1]
        );
        int text_length;
        text_position_descriptor_t *measure_result_primary = font_measure_text(
            indicator_text, formats[0], 0, &text_length, NULL
        );
        text_position_descriptor_t *measure_result_secondary = font_measure_text(
            indicator_text, formats[1], 0, NULL, NULL
        );
        display_vector_translate(&indicator_position_primary, 1, 1);
        display_vector_translate(&indicator_position_secondary, 1, 1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index < 2)
            display_draw_text(0, orientation, indicator_position_primary.x,
                indicator_position_primary.y, measure_result_primary, text_length,
                DISPLAY_COLOR_BLACK, formats[0]);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index >= 2)
            display_draw_text(1, orientation, indicator_position_secondary.x,
                indicator_position_secondary.y, measure_result_secondary, text_length,
                DISPLAY_COLOR_BLACK, formats[1]);
        display_vector_translate(&indicator_position_primary, -1, -1);
        display_vector_translate(&indicator_position_secondary, -1, -1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index < 2)
            display_draw_text(0, orientation, indicator_position_primary.x,
                indicator_position_primary.y, measure_result_primary, text_length,
                DISPLAY_COLOR_WHITE, formats[0]);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED || index >= 2)
            display_draw_text(1, orientation, indicator_position_secondary.x,
                indicator_position_secondary.y, measure_result_secondary, text_length,
                DISPLAY_COLOR_WHITE, formats[1]);
        free(measure_result_primary);
        free(measure_result_secondary);
    }
    display_update(0, orientation, &bottom_rect_primary);
    display_update(1, orientation, &bottom_rect_secondary);
}

static void ui_settings_draw_confirm(char *title, display_format_t formats[],
                                     display_orientation_t orientation, bool confirm)
{
    char *ok_text = wkc_translations_get_string("settings_ok");
    char *cancel_text = wkc_translations_get_string("settings_cancel");
    int padding = 6, spacing = 12;
    int logical_width = orientation == DISPLAY_ORIENTATION_HORIZONTAL ? DISPLAY_WIDTH_PAL :
                        DISPLAY_HEIGHT_PAL;
    int logical_height = orientation == DISPLAY_ORIENTATION_HORIZONTAL ? DISPLAY_HEIGHT_PAL :
                         DISPLAY_WIDTH_PAL;
    display_vector_t title_size, ok_size, cancel_size;
    int title_length, ok_length, cancel_length;
    free(font_measure_text(title, DISPLAY_FORMAT_PAL, 0, &title_length, &title_size));
    free(font_measure_text(ok_text, DISPLAY_FORMAT_PAL, 0, &ok_length, &ok_size));
    free(font_measure_text(cancel_text, DISPLAY_FORMAT_PAL, 0, &cancel_length, &cancel_size));
    text_position_descriptor_t *title_result_primary = font_measure_text(
        title, formats[0], 0, NULL, NULL
    );
    text_position_descriptor_t *title_result_secondary = font_measure_text(
        title, formats[1], 0, NULL, NULL
    );
    text_position_descriptor_t *ok_result_primary = font_measure_text(
        ok_text, formats[0], 0, NULL, NULL
    );
    text_position_descriptor_t *ok_result_secondary = font_measure_text(
        ok_text, formats[1], 0, NULL, NULL
    );
    text_position_descriptor_t *cancel_result_primary = font_measure_text(
        cancel_text, formats[0], 0, NULL, NULL
    );
    text_position_descriptor_t *cancel_result_secondary = font_measure_text(
        cancel_text, formats[1], 0, NULL, NULL
    );

    display_vector_t messagebox_size;
    if (orientation == DISPLAY_ORIENTATION_HORIZONTAL)
    {
        messagebox_size = (display_vector_t) {
            .x = (int)fmaxf(title_size.x, ok_size.x + cancel_size.x + spacing + padding * 2),
            .y = padding + title_size.y + ok_size.y
        };

    }
    else
    {
        messagebox_size = (display_vector_t) {
            .x = (int)fmaxf(fmaxf(title_size.x, ok_size.x + padding * 2), cancel_size.x + padding * 2),
            .y = padding * 2 + title_size.y + ok_size.y + cancel_size.y
        };
    }
    display_rect_t messagebox_border_rect = {
        .x = (logical_width - messagebox_size.x) / 2 - padding,
        .y = (logical_height - messagebox_size.y) / 2 - padding,
        .width = messagebox_size.x + padding * 2,
        .height = messagebox_size.y + padding * 2
    };
    display_rect_t messagebox_border_primary = display_rect_compensation(
        &messagebox_border_rect, formats[0]
    );
    display_rect_t messagebox_border_secondary = display_rect_compensation(
        &messagebox_border_rect, formats[1]
    );
    if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
        display_fill_rounded_rect(0, orientation, &messagebox_border_primary,
                                  DISPLAY_COLOR_TRANSPARENT, 6);
    display_fill_rounded_rect(1, orientation, &messagebox_border_secondary,
                              DISPLAY_COLOR_TRANSPARENT, 6);
    display_rect_translate(&messagebox_border_primary, 1, 1);
    display_rect_translate(&messagebox_border_secondary, 1, 1);
    if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
        display_draw_rounded_rect(0, orientation, &messagebox_border_primary,
            DISPLAY_COLOR_BLACK, 6, 2);
    display_draw_rounded_rect(1, orientation, &messagebox_border_secondary,
        DISPLAY_COLOR_BLACK, 6, 2);
    display_rect_translate(&messagebox_border_primary, -1, -1);
    display_rect_translate(&messagebox_border_secondary, -1, -1);
    if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
        display_draw_rounded_rect(0, orientation, &messagebox_border_primary,
            DISPLAY_COLOR_WHITE, 6, 2);
    display_draw_rounded_rect(1, orientation, &messagebox_border_secondary,
        DISPLAY_COLOR_WHITE, 6, 2);

    // Draw title
    display_vector_t draw_origin = {
        .x = (logical_width - title_size.x) / 2,
        .y = messagebox_border_rect.y + padding
    };
    display_vector_t draw_origin_primary = display_vector_compensation(
        &draw_origin, formats[0]
    );
    display_vector_t draw_origin_secondary = display_vector_compensation(
        &draw_origin, formats[1]
    );
    display_vector_translate(&draw_origin_primary, 1, 1);
    display_vector_translate(&draw_origin_secondary, 1, 1);
    if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
        display_draw_text(0, orientation, draw_origin_primary.x, draw_origin_primary.y,
            title_result_primary, title_length, DISPLAY_COLOR_BLACK, formats[0]);
    display_draw_text(1, orientation, draw_origin_secondary.x, draw_origin_secondary.y,
        title_result_secondary, title_length, DISPLAY_COLOR_BLACK, formats[1]);
    display_vector_translate(&draw_origin_primary, -1, -1);
    display_vector_translate(&draw_origin_secondary, -1, -1);
    if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
        display_draw_text(0, orientation, draw_origin_primary.x, draw_origin_primary.y,
            title_result_primary, title_length, DISPLAY_COLOR_WHITE, formats[0]);
    display_draw_text(1, orientation, draw_origin_secondary.x, draw_origin_secondary.y,
        title_result_secondary, title_length, DISPLAY_COLOR_WHITE, formats[1]);

    // Draw buttons
    if (orientation == DISPLAY_ORIENTATION_HORIZONTAL)
    {
        draw_origin.y += title_size.y + padding;
        draw_origin.x = (logical_width - (ok_size.x + cancel_size.x + padding * 4 + spacing)) / 2;
    }
    else
    {
        draw_origin.y += title_size.y + padding;
        draw_origin.x = (logical_width - (cancel_size.x + padding * 2)) / 2;
    }
    if (!confirm)
    {
        display_rect_t button_rect = {
            .x = draw_origin.x,
            .y = draw_origin.y,
            .width = cancel_size.x + padding * 2,
            .height = 18
        };
        display_rect_t button_rect_primary = display_rect_compensation(
            &button_rect, formats[0]
        );
        display_rect_t button_rect_secondary = display_rect_compensation(
            &button_rect, formats[1]
        );
        display_rect_translate(&button_rect_primary, 1, 1);
        display_rect_translate(&button_rect_secondary, 1, 1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
            display_fill_rounded_rect(0, orientation, &button_rect_primary,
                                      DISPLAY_COLOR_BLACK, 2);
        display_fill_rounded_rect(1, orientation, &button_rect_secondary,
                                  DISPLAY_COLOR_BLACK, 2);
        display_rect_translate(&button_rect_primary, -1, -1);
        display_rect_translate(&button_rect_secondary, -1, -1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
            display_fill_rounded_rect(0, orientation, &button_rect_primary,
                                      DISPLAY_COLOR_WHITE, 2);
        display_fill_rounded_rect(1, orientation, &button_rect_secondary,
                                  DISPLAY_COLOR_WHITE, 2);
    }
    draw_origin.x += padding;
    draw_origin_primary = display_vector_compensation(
        &draw_origin, formats[0]
    );
    draw_origin_secondary = display_vector_compensation(
        &draw_origin, formats[1]
    );
    if (confirm)
    {
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
            display_draw_text(0, orientation, draw_origin_primary.x + 1, draw_origin_primary.y + 1,
                cancel_result_primary, cancel_length, DISPLAY_COLOR_BLACK, formats[0]);
        display_draw_text(1, orientation, draw_origin_secondary.x + 1, draw_origin_secondary.y + 1,
            cancel_result_primary, cancel_length, DISPLAY_COLOR_BLACK, formats[1]);
    }
    if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
        display_draw_text(0, orientation, draw_origin_primary.x, draw_origin_primary.y,
            cancel_result_primary, cancel_length, confirm ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK,
            formats[0]);
    display_draw_text(1, orientation, draw_origin_secondary.x, draw_origin_secondary.y,
        cancel_result_secondary, cancel_length, confirm ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK,
        formats[1]);

    if (orientation == DISPLAY_ORIENTATION_HORIZONTAL)
    {
        draw_origin.x += cancel_size.x + padding + spacing;
    }
    else
    {
        draw_origin.y += cancel_size.y + padding;
        draw_origin.x = (logical_width - (ok_size.x + padding * 2)) / 2;
    }
    if (confirm)
    {
        display_rect_t button_rect = {
            .x = draw_origin.x,
            .y = draw_origin.y,
            .width = ok_size.x + padding * 2,
            .height = 18
        };
        display_rect_t button_rect_primary = display_rect_compensation(
            &button_rect, formats[0]
        );
        display_rect_t button_rect_secondary = display_rect_compensation(
            &button_rect, formats[1]
        );
        display_rect_translate(&button_rect_primary, 1, 1);
        display_rect_translate(&button_rect_secondary, 1, 1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
            display_fill_rounded_rect(0, orientation, &button_rect_primary,
                                      DISPLAY_COLOR_BLACK, 2);
        display_fill_rounded_rect(1, orientation, &button_rect_secondary,
                                  DISPLAY_COLOR_BLACK, 2);
        display_rect_translate(&button_rect_primary, -1, -1);
        display_rect_translate(&button_rect_secondary, -1, -1);
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
            display_fill_rounded_rect(0, orientation, &button_rect_primary,
                                      DISPLAY_COLOR_WHITE, 2);
        display_fill_rounded_rect(1, orientation, &button_rect_secondary,
                                  DISPLAY_COLOR_WHITE, 2);
    }
    draw_origin.x += padding;
    draw_origin_primary = display_vector_compensation(
        &draw_origin, formats[0]
    );
    draw_origin_secondary = display_vector_compensation(
        &draw_origin, formats[1]
    );
    if (!confirm)
    {
        if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
            display_draw_text(0, orientation, draw_origin_primary.x + 1, draw_origin_primary.y + 1,
                ok_result_primary, ok_length, DISPLAY_COLOR_BLACK, formats[0]);
        display_draw_text(1, orientation, draw_origin_secondary.x + 1, draw_origin_secondary.y + 1,
            ok_result_primary, ok_length, DISPLAY_COLOR_BLACK, formats[1]);
    }
    if (orientation != DISPLAY_ORIENTATION_VERTICAL_TILED)
        display_draw_text(0, orientation, draw_origin_primary.x, draw_origin_primary.y,
            ok_result_primary, ok_length, confirm ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE,
            formats[0]);
    display_draw_text(1, orientation, draw_origin_secondary.x, draw_origin_secondary.y,
        ok_result_secondary, ok_length, confirm ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE,
        formats[1]);

    display_rect_expand(&messagebox_border_primary, 2, 2);
    display_rect_expand(&messagebox_border_secondary, 2, 2);
    display_update(0, orientation, &messagebox_border_primary);
    display_update(1, orientation, &messagebox_border_secondary);

    free(title_result_primary);
    free(title_result_secondary);
    free(ok_result_primary);
    free(ok_result_secondary);
    free(cancel_result_primary);
    free(cancel_result_secondary);
}

static void ui_settings_draw_menu(ui_settings_t *settings, display_format_t *formats,
                                  display_orientation_t orientation)
{
    int menu_count = ui_settings_get_menu_count(settings);
    if (menu_count <= 0) return;
    ui_menu_item_t *menu = settings->current_menu[menu_count - 1];
    int item_count = ui_common_menu_get_item_count(menu);
    if (item_count <= 0) return;
    int selected = settings->selected_index[menu_count - 1];
    if (selected >= item_count) selected = item_count - 1;
    if (selected < 0) selected = 0;

    int logical_width = orientation == DISPLAY_ORIENTATION_HORIZONTAL ? DISPLAY_WIDTH_PAL :
                        DISPLAY_HEIGHT_PAL;
    int logical_height = orientation == DISPLAY_ORIENTATION_HORIZONTAL ? DISPLAY_HEIGHT_PAL :
                         DISPLAY_WIDTH_PAL;
    display_vector_t center = { .x = logical_width / 2, .y = logical_height / 2 };
    // The root menu is drawn on the first screen of the tiled output, the sub
    // menus on the second one (same as before the refactor)
    int prefer_side = menu_count == 1 ? 0 : 1;

    if (settings->show || settings->last_selected_index < 0)
    {
        bool tiled = orientation == DISPLAY_ORIENTATION_VERTICAL_TILED;
        // In the tiled output the first screen shows the parent menu. Opening a
        // sub menu from it keeps that screen intact, so only the second screen
        // is refreshed; a real full repaint (orientation change, page refresh)
        // erases and redraws both screens.
        if (tiled && menu_count >= 2 && !settings->parent_needs_repaint)
        {
            DISPLAY_CLEAR_SCREEN(1);
        }
        else
        {
            DISPLAY_CLEAR_SCREEN(0);
            DISPLAY_CLEAR_SCREEN(1);
            if (tiled && menu_count >= 2)
            {
                ui_menu_item_t *parent_menu = settings->current_menu[menu_count - 2];
                int parent_count = ui_common_menu_get_item_count(parent_menu);
                int parent_selected = parent_count > 0 ?
                                      settings->selected_index[menu_count - 2] : 0;
                if (parent_selected >= parent_count) parent_selected = parent_count - 1;
                if (parent_selected < 0) parent_selected = 0;
                if (parent_count > 0)
                    ui_common_menu_draw_item(parent_menu, parent_selected, true, 0, &center,
                                             UI_MENU_ALIGN_CENTER, UI_MENU_ALIGN_CENTER,
                                             formats, orientation, true, 0);
            }
        }
        ui_common_menu_draw_item(menu, selected, true, prefer_side, &center,
                                 UI_MENU_ALIGN_CENTER, UI_MENU_ALIGN_CENTER,
                                 formats, orientation, true, 0);
    }
    else
    {
        if (settings->last_selected_index != selected)
            ui_common_menu_draw_item(menu, settings->last_selected_index, false, prefer_side,
                                     &center, UI_MENU_ALIGN_CENTER, UI_MENU_ALIGN_CENTER,
                                     formats, orientation, false, 0);
        ui_common_menu_draw_item(menu, selected, true, prefer_side, &center,
                                 UI_MENU_ALIGN_CENTER, UI_MENU_ALIGN_CENTER,
                                 formats, orientation, false, 0);
    }
}

static void ui_settings_on_draw(ui_settings_t *settings, display_format_t *formats,
                                display_orientation_t orientation)
{
    if (!settings->show && !settings->draw_request) return;

    bool full_repaint = settings->show;
    if (settings->special == UI_SETTINGS_SPECIAL_POSITION)
    {
        wkc_settings_t *current_settings = wkc_settings_get_current();
        int index = settings->special_index;
        ui_settings_draw_display_position(index, formats, orientation,
            current_settings->display.position[index / 2].output_timing, full_repaint);
    }
    else if (settings->special == UI_SETTINGS_SPECIAL_CONFIRM)
    {
        // A full repaint (e.g. the phone changed a setting) has to refresh the
        // menu below the dialog as well
        if (full_repaint)
            ui_settings_draw_menu(settings, formats, orientation);
        char *title = wkc_translations_get_string(settings->confirm_target == 1 ?
            "settings_confirm_clear_devices" : "settings_confirm_factory_reset");
        ui_settings_draw_confirm(title, formats, orientation, settings->special_index != 0);
    }
    else
    {
        ui_settings_draw_menu(settings, formats, orientation);
    }

    settings->show = false;
    settings->draw_request = false;
    int menu_count = ui_settings_get_menu_count(settings);
    if (menu_count > 0)
        settings->last_selected_index = settings->selected_index[menu_count - 1];
}

static void ui_settings_on_show(ui_settings_t *settings)
{
    ui_settings_menu_free_all(settings);
    settings->show = true;
    settings->draw_request = true;
    settings->last_selected_index = -1;
    settings->special = UI_SETTINGS_SPECIAL_NONE;
    settings->special_index = 0;
    settings->confirm_target = 0;
    ui_settings_menu_push(settings, ui_settings_build_main_menu());
    settings->parent_needs_repaint = true;
}

// Rebuilds the open menu path so labels (translations) and values are re-read.
// Needed when the language changes or when the phone changes a setting while
// this page is open (both end up as a refresh request from the shell).
static void ui_settings_rebuild_menus(ui_settings_t *settings)
{
    int depth = ui_settings_get_menu_count(settings);
    int selection[UI_SETTINGS_MENU_MAX_DEPTH];
    for (int i = 0; i < depth; i++)
        selection[i] = settings->selected_index[i];
    ui_settings_menu_free_all(settings);
    if (depth <= 0)
        return;
    ui_settings_menu_push(settings, ui_settings_build_main_menu());
    if (depth >= 2)
    {
        int root = selection[0];
        ui_menu_item_t *sub_menu = root == 0 ? ui_settings_build_display_menu() :
                                   root == 1 ? ui_settings_build_misc_menu() :
                                   ui_settings_build_advanced_menu();
        ui_settings_menu_push(settings, sub_menu);
    }
    for (int i = 0; i < depth; i++)
    {
        int item_count = ui_common_menu_get_item_count(settings->current_menu[i]);
        settings->selected_index[i] = item_count > 0 ?
                                      clamp(selection[i], 0, item_count - 1) : 0;
    }
}

static void ui_settings_on_refresh(ui_settings_t *settings)
{
    ui_settings_rebuild_menus(settings);
    settings->parent_needs_repaint = true;
    settings->show = true;
    settings->draw_request = true;
    settings->last_selected_index = -1;
}

static void ui_settings_on_key_event(ui_settings_t *settings, int key_code)
{
    wkc_settings_t *current_settings = wkc_settings_get_current();
    display_orientation_t orientation = ui_shell_get_orientation(settings->base.parent);

    // Display position editor (full screen)
    if (settings->special == UI_SETTINGS_SPECIAL_POSITION)
    {
        if (key_code == UI_KEY_CODE_EXIT ||
            (key_code == UI_KEY_CODE_OK && settings->special_index >= 3))
        {
            settings->special = UI_SETTINGS_SPECIAL_NONE;
            settings->show = true;
            settings->draw_request = true;
            settings->last_selected_index = -1;
            return;
        }
        if (key_code == UI_KEY_CODE_OK)
        {
            settings->special_index += 1;
            settings->draw_request = true;
            return;
        }
        if (settings->special_index % 2 == 0)
        {
            int *x_offset = &current_settings->display.position[
                settings->special_index / 2].x_offset;
            int *y_offset = &current_settings->display.position[
                settings->special_index / 2].y_offset;
            if (orientation == DISPLAY_ORIENTATION_HORIZONTAL)
            {
                if (key_code == UI_KEY_CODE_UP)
                    *y_offset -= 1;
                if (key_code == UI_KEY_CODE_DOWN)
                    *y_offset += 1;
                if (key_code == UI_KEY_CODE_LEFT)
                    *x_offset -= 1;
                if (key_code == UI_KEY_CODE_RIGHT)
                    *x_offset += 1;
            }
            else
            {
                if (key_code == UI_KEY_CODE_UP)
                    *x_offset += 1;
                if (key_code == UI_KEY_CODE_DOWN)
                    *x_offset -= 1;
                if (key_code == UI_KEY_CODE_LEFT)
                    *y_offset -= 1;
                if (key_code == UI_KEY_CODE_RIGHT)
                    *y_offset += 1;
            }
            *y_offset = clamp(*y_offset, -16, 16);
            *x_offset = clamp(*x_offset, -128, 127);
            display_settings_update();
            wkc_settings_save();
            return;
        }
        int *output_timing = &current_settings->display.position[
            settings->special_index / 2].output_timing;
        if (key_code == UI_KEY_CODE_LEFT)
            *output_timing -= 1;
        if (key_code == UI_KEY_CODE_RIGHT)
            *output_timing += 1;
        *output_timing = clamp(*output_timing, -15, 15);
        settings->draw_request = true;
        wkc_settings_save();
        return;
    }

    // Confirmation dialog (full screen)
    if (settings->special == UI_SETTINGS_SPECIAL_CONFIRM)
    {
        if (key_code == UI_KEY_CODE_EXIT || key_code == UI_KEY_CODE_OK)
        {
            if (key_code == UI_KEY_CODE_OK && settings->special_index != 0)
            {
                if (settings->confirm_target == 1)
                {
                    wkc_security_load_default();
                    esp_restart();
                }
                else
                {
                    wkc_userprofile_load_default();
                    wkc_settings_load_default();
                    wkc_security_load_default();
                    camera_control_reset();
                    esp_restart();
                }
                return;
            }
            settings->special = UI_SETTINGS_SPECIAL_NONE;
            settings->show = true;
            settings->draw_request = true;
            settings->last_selected_index = -1;
            return;
        }
        if ((orientation == DISPLAY_ORIENTATION_HORIZONTAL && (key_code == UI_KEY_CODE_LEFT ||
             key_code == UI_KEY_CODE_RIGHT)) ||
            (orientation != DISPLAY_ORIENTATION_HORIZONTAL && (key_code == UI_KEY_CODE_UP ||
             key_code == UI_KEY_CODE_DOWN)))
        {
            settings->special_index = !settings->special_index;
            settings->draw_request = true;
        }
        return;
    }

    int menu_count = ui_settings_get_menu_count(settings);
    if (menu_count <= 0) return;
    ui_menu_item_t *menu = settings->current_menu[menu_count - 1];
    int item_count = ui_common_menu_get_item_count(menu);
    if (item_count <= 0) return;
    int *selected = &settings->selected_index[menu_count - 1];

    if (key_code == UI_KEY_CODE_EXIT)
    {
        if (menu_count <= 1)
        {
            ui_page_t *home = ui_shell_find_page(settings->base.parent, UI_PAGE_TYPE_HOME);
            ui_shell_show_page(settings->base.parent, home);
        }
        else
        {
            // Back to the previous menu
            ui_settings_menu_pop(settings);
        }
        return;
    }

    if (key_code == UI_KEY_CODE_DOWN || key_code == UI_KEY_CODE_UP)
    {
        // Wraps around, as the page always did
        settings->last_selected_index = *selected;
        *selected = (*selected + (key_code == UI_KEY_CODE_DOWN ? 1 : item_count - 1)) %
                    item_count;
        settings->draw_request = true;
        return;
    }

    ui_menu_item_t *item = &menu[*selected];
    settings->last_selected_index = *selected;
    if (key_code == UI_KEY_CODE_LEFT || key_code == UI_KEY_CODE_RIGHT)
    {
        if (item->type == UI_MENU_ITEM_PICKER)
        {
            if (item->count > 1)
            {
                // Pickers wrap around (unlike the camera menu)
                item->current_value = (item->current_value + item->count +
                    (key_code == UI_KEY_CODE_RIGHT ? 1 : item->count - 1)) % item->count;
            }
            // count == 1 entries (e.g. the chat code) act on every press
            if (item->action != NULL)
                item->action(item, (ui_page_t*)settings);
            settings->draw_request = true;
        }
        return;
    }
    if (key_code == UI_KEY_CODE_OK)
    {
        if (item->type == UI_MENU_ITEM_SWITCH)
        {
            item->current_value = !item->current_value;
            if (item->action != NULL)
                item->action(item, (ui_page_t*)settings);
            settings->draw_request = true;
        }
        else if (item->type == UI_MENU_ITEM_LABEL && item->action != NULL)
        {
            item->action(item, (ui_page_t*)settings);
        }
    }
}

ui_page_t *ui_settings_create()
{
    ui_settings_t *settings = (ui_settings_t*)calloc(1, sizeof(ui_settings_t));
    settings->base.type = UI_PAGE_TYPE_SETTINGS;
    settings->base.on_show = (ui_page_event_t)ui_settings_on_show;
    settings->base.on_draw = (ui_page_draw_event_t)ui_settings_on_draw;
    settings->base.on_key_event = (ui_page_key_event_t)ui_settings_on_key_event;
    settings->base.on_mainloop = NULL;
    settings->base.on_refresh = (ui_page_event_t)ui_settings_on_refresh;
    return (ui_page_t*)settings;
}
