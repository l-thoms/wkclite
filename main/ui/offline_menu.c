#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "ui_common.h"
#include "shell.h"
#include "page.h"
#include "offline_menu.h"
#include "display/display_control.h"
#include "profile/translations.h"
#include "profile/settings.h"
#include "peripherals/lock.h"
#include "peripherals/pwm.h"
#include "peripherals/battery_calibration.h"
#include "io/io_extend.h"
#include "io/power_key.h"
#include "protocol/ble.h"

// Brightness pulses of the display controller (same as the main menu)
#define BRIGHTNESS_UP_GPIO 17
#define BRIGHTNESS_DOWN_GPIO 16
#define BRIGHTNESS_MIRROR_GPIO 20
// Consecutive presses closer than this are treated as one gesture
#define BRIGHTNESS_GESTURE_GAP_US 500000
// Number of presses that triggers the mirror flip
#define BRIGHTNESS_MIRROR_PRESSES 10
// Main loop interval while the brightness gesture is running
#define BRIGHTNESS_POLL_MS 50

// Same metrics as the vertical main menu
#define OFFLINE_MENU_ITEM_HEIGHT 28
#define OFFLINE_MENU_ITEM_WIDTH 180
#define OFFLINE_MENU_WIDTH 192
#define OFFLINE_MENU_BORDER_RADIUS 8

typedef enum
{
    OFFLINE_ITEM_STANDBY,
    OFFLINE_ITEM_FAN,
    OFFLINE_ITEM_EYE,
    OFFLINE_ITEM_LOCK,
    OFFLINE_ITEM_BRIGHTNESS,
    OFFLINE_ITEM_CHAT_CODE,
    OFFLINE_ITEM_DISCONNECT,
    OFFLINE_ITEM_DEVICE_INFO,
    OFFLINE_ITEM_COUNT
} offline_item_t;

typedef struct
{
    ui_page_t base;
    int selected_index;
    int last_selected_index;
    int last_drawn_count;
    // Visible entries, frozen when the menu opens: a client connecting or
    // disconnecting while the menu is shown must not add or remove entries
    uint32_t item_mask;
    bool draw_request;
    bool brightness_tweak;
    int brightness_count;
    int64_t brightness_last_time;
    // Fingerprint of the values shown in the entries, used to refresh the menu
    // when they change (e.g. the lock changed by another page or by the app)
    int state_signature;
} ui_offline_menu_t;

// Static rules for the entries that depend on the runtime state
static bool offline_item_supported(offline_item_t item)
{
    switch (item)
    {
        case OFFLINE_ITEM_CHAT_CODE:
            return wkc_settings_get_current()->chat_status == WKC_CHAT_STATUS_LIMITED;
        case OFFLINE_ITEM_DISCONNECT:
            return protocol_ble_is_device_connected();
        default:
            return true;
    }
}

static int offline_item_count(ui_offline_menu_t *menu)
{
    int count = 0;
    for (int i = 0; i < OFFLINE_ITEM_COUNT; i++)
        if (menu->item_mask & (1u << i)) count += 1;
    return count;
}

static offline_item_t offline_item_at(ui_offline_menu_t *menu, int index)
{
    for (int i = 0; i < OFFLINE_ITEM_COUNT; i++)
        if (menu->item_mask & (1u << i))
        {
            if (index == 0) return (offline_item_t)i;
            index -= 1;
        }
    return OFFLINE_ITEM_STANDBY;
}

// Evaluates the entry list once and keeps it stable while the menu is open
static void offline_menu_freeze_items(ui_offline_menu_t *menu)
{
    uint32_t mask = 0;
    for (int i = 0; i < OFFLINE_ITEM_COUNT; i++)
        if (offline_item_supported((offline_item_t)i)) mask |= 1u << i;
    menu->item_mask = mask;
    int count = offline_item_count(menu);
    if (count <= 0) menu->selected_index = 0;
    else if (menu->selected_index >= count) menu->selected_index = count - 1;
}

static void offline_menu_item_label(ui_offline_menu_t *menu, offline_item_t item,
                                    char *text, char *image)
{
    switch (item)
    {
        case OFFLINE_ITEM_STANDBY:
            strcpy(text, wkc_translations_get_string("menu_standby"));
            strcpy(image, "standby");
            break;
        case OFFLINE_ITEM_FAN:
        {
            int level = pwm_device_get_fan_level();
            if (!level)
                sprintf(text, "%s: %s", wkc_translations_get_string("menu_fan"),
                        wkc_translations_get_string("menu_off"));
            else
                sprintf(text, "%s: %d", wkc_translations_get_string("menu_fan"), level);
            strcpy(image, level ? "fan" : "fan_disabled");
            break;
        }
        case OFFLINE_ITEM_EYE:
        {
            int level = pwm_device_get_eye_level();
            if (!level)
                sprintf(text, "%s: %s", wkc_translations_get_string("menu_eye"),
                        wkc_translations_get_string("menu_off"));
            else
                sprintf(text, "%s: %d", wkc_translations_get_string("menu_eye"), level);
            strcpy(image, level ? "eye" : "eye_disabled");
            break;
        }
        case OFFLINE_ITEM_LOCK:
            sprintf(text, "%s: %s", wkc_translations_get_string("menu_lock"),
                    wkc_translations_get_string(lock_get_state() ? "menu_locked" :
                                                "menu_unlocked"));
            strcpy(image, lock_get_state() ? "lock" : "unlock");
            break;
        case OFFLINE_ITEM_BRIGHTNESS:
            strcpy(text, wkc_translations_get_string(menu->brightness_tweak ?
                   "menu_tweaking" : "menu_brightness"));
            strcpy(image, "brightness");
            break;
        case OFFLINE_ITEM_CHAT_CODE:
            sprintf(text, "%s: %06d",
                    wkc_translations_get_string("settings_chat_passkey"),
                    wkc_settings_get_current()->chat_passkey);
            strcpy(image, "chat");
            break;
        case OFFLINE_ITEM_DISCONNECT:
            strcpy(text, wkc_translations_get_string("menu_disconnect_device"));
            strcpy(image, "disconnect");
            break;
        case OFFLINE_ITEM_DEVICE_INFO:
            strcpy(text, wkc_translations_get_string("menu_device_info"));
            strcpy(image, "info");
            break;
        default:
            text[0] = '\0';
            image[0] = '\0';
            break;
    }
}

// Logical centre of the screen for the active orientation
static void offline_menu_center(display_orientation_t orientation, int *center_x, int *center_y)
{
    bool horizontal = orientation == DISPLAY_ORIENTATION_HORIZONTAL;
    *center_x = horizontal ? DISPLAY_WIDTH_PAL / 2 : DISPLAY_HEIGHT_PAL / 2;
    *center_y = horizontal ? DISPLAY_HEIGHT_PAL / 2 : DISPLAY_WIDTH_PAL / 2;
}

// Border of the popup, same proportions as the vertical main menu
static void offline_menu_get_frame(display_orientation_t orientation, int count,
                                   display_rect_t *rect)
{
    int center_x, center_y;
    offline_menu_center(orientation, &center_x, &center_y);
    int height = OFFLINE_MENU_ITEM_HEIGHT * count + 12;
    rect->x = center_x - OFFLINE_MENU_WIDTH / 2;
    rect->y = center_y - height / 2;
    rect->width = OFFLINE_MENU_WIDTH;
    rect->height = height;
}

// Entry rectangle, derived from the border so every entry count stays aligned
// (6px padding between the border and the first/last entry)
static display_rect_t offline_menu_item_rect(display_orientation_t orientation, int index,
                                             int count)
{
    display_rect_t frame;
    offline_menu_get_frame(orientation, count, &frame);
    return (display_rect_t) {
        .x = frame.x + (OFFLINE_MENU_WIDTH - OFFLINE_MENU_ITEM_WIDTH) / 2,
        .y = frame.y + 6 + OFFLINE_MENU_ITEM_HEIGHT * index,
        .width = OFFLINE_MENU_ITEM_WIDTH,
        .height = OFFLINE_MENU_ITEM_HEIGHT
    };
}

// Clears the popup body of the interface layer so the video of the hardware OSD
// compositor shows through again. Exactly the extent of the border, nothing more.
static void offline_menu_fill_body(int screen, display_orientation_t orientation,
                                   display_format_t format, int count)
{
    display_rect_t frame;
    offline_menu_get_frame(orientation, count, &frame);
    display_rect_t frame_compensated = display_rect_compensation(&frame, format);
    display_fill_rounded_rect(screen, orientation, &frame_compensated,
                              DISPLAY_COLOR_TRANSPARENT, OFFLINE_MENU_BORDER_RADIUS);
}

// Clears the whole popup (border, shadow and body) of the interface layer
static void offline_menu_clear(ui_offline_menu_t *menu)
{
    menu->last_drawn_count = 0;
}

// Draws one entry on the interface layer and commits only its own region
static void offline_menu_draw_item(ui_offline_menu_t *menu, int index, bool highlight,
                                   display_format_t *formats, display_orientation_t orientation,
                                   int count)
{
    if (index < 0 || index >= count) return;
    offline_item_t item = offline_item_at(menu, index);
    char text[50] = { 0 };
    char image_name[20] = { 0 };
    offline_menu_item_label(menu, item, text, image_name);

    display_rect_t item_rect = offline_menu_item_rect(orientation, index, count);
    bool tiled = orientation == DISPLAY_ORIENTATION_VERTICAL_TILED;

    for (int screen = 0; screen < 2; screen++)
    {
        if (screen == 0 && tiled) continue;
        display_format_t format = formats[screen];

        int text_length = 0;
        display_vector_t text_size;
        text_position_descriptor_t *descriptor = font_measure_text(text, format, 0,
                                                 &text_length, &text_size);
        char image_path[60] = { 0 };
        int image_width = 0;
        display_get_icon(image_name, format, false, image_path, &image_width);

        // Region of this entry (the highlight plate can stick out by one pixel)
        display_rect_t clear = item_rect;
        display_rect_expand(&clear, 1, 1);
        display_rect_t clear_compensated = display_rect_compensation(&clear, format);
        display_fill_rect(screen, orientation, &clear_compensated, DISPLAY_COLOR_TRANSPARENT);

        if (highlight)
        {
            display_rect_t plate = item_rect;
            display_rect_expand(&plate, -2, -2);
            display_rect_t plate_compensated = display_rect_compensation(&plate, format);
            // Same as the vertical main menu: black shadow, then white plate
            display_rect_t plate_shadow = plate_compensated;
            display_rect_translate(&plate_shadow, 1, 1);
            display_fill_rounded_rect(screen, orientation, &plate_shadow, DISPLAY_COLOR_BLACK, 2);
            display_fill_rounded_rect(screen, orientation, &plate_compensated,
                                      DISPLAY_COLOR_WHITE, 2);
        }

        display_vector_t icon_origin = display_coordinate_compensation(item_rect.x + 2,
                                        item_rect.y + 2, format);
        display_vector_t text_origin = display_coordinate_compensation(item_rect.x + 30,
                                        item_rect.y + 4, format);
        if (!highlight)
        {
            display_draw_image(screen, orientation, icon_origin.x + 1, icon_origin.y + 1,
                               image_path, image_width, DISPLAY_COLOR_BLACK);
            display_draw_text(screen, orientation, text_origin.x + 1, text_origin.y + 1,
                              descriptor, text_length, DISPLAY_COLOR_BLACK, format);
        }
        display_draw_image(screen, orientation, icon_origin.x, icon_origin.y, image_path,
                           image_width, highlight ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE);
        display_draw_text(screen, orientation, text_origin.x, text_origin.y, descriptor,
                          text_length, highlight ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE,
                          format);

        free(descriptor);
        display_update(screen, orientation, &clear_compensated);
    }
}

// Draws the border and commits the whole popup region (full refresh only)
static void offline_menu_draw_frame(ui_offline_menu_t *menu, display_format_t *formats,
                                    display_orientation_t orientation, int count)
{
    display_rect_t frame;
    offline_menu_get_frame(orientation, count, &frame);
    display_rect_t update = frame;
    display_rect_expand(&update, 2, 2);   // include the border and its shadow
    bool tiled = orientation == DISPLAY_ORIENTATION_VERTICAL_TILED;

    for (int screen = 0; screen < 2; screen++)
    {
        if (screen == 0 && tiled) continue;
        display_format_t format = formats[screen];
        display_rect_t update_compensated = display_rect_compensation(&update, format);

        // Transparent body with exactly the extent of the border: the video
        // beneath the popup stays visible, nothing outside the border is drawn
        offline_menu_fill_body(screen, orientation, format, count);

        display_rect_t frame_compensated = display_rect_compensation(&frame, format);
        display_rect_t frame_shadow = frame_compensated;
        display_rect_translate(&frame_shadow, 1, 1);
        display_draw_rounded_rect(screen, orientation, &frame_shadow, DISPLAY_COLOR_BLACK,
                                  OFFLINE_MENU_BORDER_RADIUS, 2);
        display_draw_rounded_rect(screen, orientation, &frame_compensated, DISPLAY_COLOR_WHITE,
                                  OFFLINE_MENU_BORDER_RADIUS, 2);
        display_update(screen, orientation, &update_compensated);
    }
    menu->last_drawn_count = count;
}

static void offline_menu_draw(ui_offline_menu_t *menu, display_format_t *formats,
                              display_orientation_t orientation)
{
    int count = offline_item_count(menu);
    if (count <= 0) return;
    if (menu->selected_index >= count) menu->selected_index = count - 1;
    if (menu->selected_index < 0) menu->selected_index = 0;

    if (menu->last_selected_index < 0)
    {
        // Full redraw: border plus every entry
        offline_menu_draw_frame(menu, formats, orientation, count);
        for (int i = 0; i < count; i++)
            offline_menu_draw_item(menu, i, i == menu->selected_index, formats, orientation,
                                   count);
    }
    else if (menu->last_selected_index != menu->selected_index)
    {
        // Incremental: only the previously and the newly highlighted entries
        offline_menu_draw_item(menu, menu->last_selected_index, false, formats, orientation,
                               count);
        offline_menu_draw_item(menu, menu->selected_index, true, formats, orientation, count);
    }
    else
    {
        // Value of the highlighted entry changed (e.g. fan level)
        offline_menu_draw_item(menu, menu->selected_index, true, formats, orientation, count);
    }
    menu->last_selected_index = menu->selected_index;
}

static void offline_menu_brightness_pulse(int gpio)
{
    gpio_write(GPIO_NUM_EXTEND | gpio, 1);
}

// Ends the running brightness gesture: 1 press = brighter, 2 = dimmer, 10 = flip
static void offline_menu_brightness_finish(ui_offline_menu_t *menu)
{
    int presses = menu->brightness_count;
    menu->brightness_count = 0;
    if (presses == 1)
        offline_menu_brightness_pulse(BRIGHTNESS_UP_GPIO);
    else if (presses == 2)
        offline_menu_brightness_pulse(BRIGHTNESS_DOWN_GPIO);
    else if (presses >= BRIGHTNESS_MIRROR_PRESSES)
        offline_menu_brightness_pulse(BRIGHTNESS_MIRROR_GPIO);
}

// Fingerprint of every value shown by the entries
static int offline_menu_signature(ui_offline_menu_t *menu)
{
    int signature = pwm_device_get_fan_level() * 4 + pwm_device_get_eye_level();
    signature = signature * 2 + (lock_get_state() ? 1 : 0);
    signature = signature * 2 + (menu->brightness_tweak ? 1 : 0);
    signature = signature * 16 + offline_item_count(menu);
    signature = signature * 1000003 + wkc_settings_get_current()->chat_passkey;
    return signature;
}

static void offline_menu_close(ui_offline_menu_t *menu)
{
    offline_menu_clear(menu);
    menu->last_selected_index = -1;
    ui_shell_close_popup(menu->base.parent);
}

static void offline_menu_activate(ui_offline_menu_t *menu, offline_item_t item)
{
    ui_shell_t *shell = menu->base.parent;
    switch (item)
    {
        case OFFLINE_ITEM_STANDBY:
            if (battery_calibration_is_calibrating())
            {
                ui_shell_show_toast(shell, wkc_translations_get_string(
                    "menu_standby_disable_calibrate"), 5);
                break;
            }
            offline_menu_close(menu);
            power_key_standby();
            break;
        case OFFLINE_ITEM_FAN:
            if (battery_calibration_is_calibrating())
                ui_shell_show_toast(ui_shell_get_current(), wkc_translations_get_string(
                    "menu_peripherals_disable_calibrate"), 5);
            else
            {
                pwm_device_toggle_fan();
                menu->draw_request = true;
                protocol_ble_notify_update_command(WKC_CATEGORY_SHORTCUT, "fan");
            }
            break;
        case OFFLINE_ITEM_EYE:
            if (battery_calibration_is_calibrating())
                ui_shell_show_toast(ui_shell_get_current(), wkc_translations_get_string(
                    "menu_peripherals_disable_calibrate"), 5);
            else
            {
                pwm_device_toggle_eye();
                menu->draw_request = true;
                protocol_ble_notify_update_command(WKC_CATEGORY_SHORTCUT, "eye");
            }
            break;
        case OFFLINE_ITEM_LOCK:
        {
            // Same flow as the lock entry of the main menu
            bool target = !lock_get_state();
            int result = lock_set(target, false);
            if (lock_get_state() == target)
                menu->draw_request = true;                 // state changed
            else if (result != 0)
                ui_shell_show_toast(shell, lock_result_to_char(result), 5);   // abnormal only
            protocol_ble_notify_update_command(WKC_CATEGORY_SHORTCUT, "lock");
            break;
        }
        case OFFLINE_ITEM_BRIGHTNESS:
            offline_menu_brightness_finish(menu);
            menu->brightness_tweak = true;
            menu->brightness_count = 0;
            menu->brightness_last_time = 0;
            menu->draw_request = true;
            break;
        case OFFLINE_ITEM_CHAT_CODE:
            // Regenerate the 6 digit passkey used for chat verification
            wkc_settings_get_current()->chat_passkey = esp_random() % (int)1e06;
            menu->draw_request = true;
            break;
        case OFFLINE_ITEM_DISCONNECT:
            protocol_ble_disconnect_current();
            offline_menu_close(menu);
            break;
        case OFFLINE_ITEM_DEVICE_INFO:
            offline_menu_close(menu);
            ui_shell_show_page(shell, ui_shell_find_page(shell, UI_PAGE_TYPE_DEVICE_INFO));
            break;
        default:
            break;
    }
}

static void offline_menu_brightness_key(ui_offline_menu_t *menu, int key_code)
{
    switch (key_code)
    {
        case UI_KEY_CODE_UP:
        case UI_KEY_CODE_RIGHT:
            offline_menu_brightness_finish(menu);
            offline_menu_brightness_pulse(BRIGHTNESS_UP_GPIO);
            break;
        case UI_KEY_CODE_DOWN:
        case UI_KEY_CODE_LEFT:
            offline_menu_brightness_finish(menu);
            offline_menu_brightness_pulse(BRIGHTNESS_DOWN_GPIO);
            break;
        case UI_KEY_CODE_GPIO0_SHORT:
        {
            // A pause longer than the gesture gap ends the previous gesture
            int64_t now = esp_timer_get_time();
            if (menu->brightness_count > 0 &&
                now - menu->brightness_last_time > BRIGHTNESS_GESTURE_GAP_US)
                offline_menu_brightness_finish(menu);
            menu->brightness_count += 1;
            menu->brightness_last_time = now;
            break;
        }
        case UI_KEY_CODE_GPIO0_LONG:
        case UI_KEY_CODE_OK:
        case UI_KEY_CODE_EXIT:
        case UI_KEY_CODE_MENU:
            offline_menu_brightness_finish(menu);
            menu->brightness_tweak = false;
            menu->draw_request = true;
            break;
        default:
            break;
    }
}

static void ui_offline_menu_on_key_event(ui_offline_menu_t *menu, int key_code)
{
    if (menu->brightness_tweak)
    {
        offline_menu_brightness_key(menu, key_code);
        return;
    }
    int count = offline_item_count(menu);
    if (count <= 0) return;

    switch (key_code)
    {
        case UI_KEY_CODE_UP:
        case UI_KEY_CODE_LEFT:
            menu->selected_index = (menu->selected_index + count - 1) % count;
            menu->draw_request = true;
            break;
        case UI_KEY_CODE_RIGHT:
        case UI_KEY_CODE_DOWN:
            menu->selected_index = (menu->selected_index + 1) % count;
            menu->draw_request = true;
            break;
        case UI_KEY_CODE_OK:
        case UI_KEY_CODE_GPIO0_LONG:
            offline_menu_activate(menu, offline_item_at(menu, menu->selected_index));
            break;
        case UI_KEY_CODE_GPIO0_SHORT:
            // Short press moves to the next entry; after the last one it closes
            if (menu->selected_index + 1 >= count)
                offline_menu_close(menu);
            else
            {
                menu->selected_index += 1;
                menu->draw_request = true;
            }
            break;
        case UI_KEY_CODE_EXIT:
            offline_menu_close(menu);
            break;
        default:
            break;
    }
}

static void ui_offline_menu_on_mainloop(ui_offline_menu_t *menu, bool on_foreground)
{
    if (!on_foreground) return;

    // The entry list is frozen; if the client went away while the disconnect
    // entry is selected, close the menu right away
    if (offline_item_at(menu, menu->selected_index) == OFFLINE_ITEM_DISCONNECT &&
        !protocol_ble_is_device_connected())
    {
        offline_menu_close(menu);
        return;
    }

    // Redraw when one of the displayed values changed from the outside
    int signature = offline_menu_signature(menu);
    if (signature != menu->state_signature)
    {
        menu->state_signature = signature;
        menu->draw_request = true;
    }

    if (!menu->brightness_tweak) return;
    // Poll fast enough to evaluate the 0.5s gesture gap in time
    ui_shell_acquire_interval(menu->base.parent, BRIGHTNESS_POLL_MS);
    if (menu->brightness_count <= 0) return;
    if (esp_timer_get_time() - menu->brightness_last_time < BRIGHTNESS_GESTURE_GAP_US) return;
    offline_menu_brightness_finish(menu);
}

static void ui_offline_menu_on_show(ui_offline_menu_t *menu)
{
    // Entries are evaluated once per opening and stay fixed until it closes
    offline_menu_freeze_items(menu);
    menu->selected_index = 0;
    menu->last_selected_index = -1;
    menu->draw_request = true;
    menu->brightness_tweak = false;
    menu->brightness_count = 0;
    menu->brightness_last_time = 0;
    menu->state_signature = offline_menu_signature(menu);
}

static void ui_offline_menu_on_refresh(ui_offline_menu_t *menu)
{
    // Orientation or format changed: repaint the whole popup
    menu->last_selected_index = -1;
    menu->draw_request = true;
}

static void ui_offline_menu_on_draw(ui_offline_menu_t *menu, display_format_t *formats,
                                    display_orientation_t orientation)
{
    if (!menu->draw_request) return;
    menu->draw_request = false;
    offline_menu_draw(menu, formats, orientation);
}

ui_page_t *ui_offline_menu_create()
{
    ui_offline_menu_t *menu = calloc(1, sizeof(ui_offline_menu_t));
    if (menu == NULL) return NULL;
    menu->base.type = UI_PAGE_TYPE_OFFLINE_MENU;
    menu->base.on_show = (ui_page_event_t)ui_offline_menu_on_show;
    menu->base.on_draw = (ui_page_draw_event_t)ui_offline_menu_on_draw;
    menu->base.on_key_event = (ui_page_key_event_t)ui_offline_menu_on_key_event;
    menu->base.on_mainloop = (ui_page_mainloop_event_t)ui_offline_menu_on_mainloop;
    menu->base.on_refresh = (ui_page_event_t)ui_offline_menu_on_refresh;
    offline_menu_freeze_items(menu);
    menu->selected_index = 0;
    menu->last_selected_index = -1;
    menu->draw_request = true;
    return (ui_page_t*)menu;
}

void ui_offline_menu_show(ui_shell_t *shell)
{
    if (shell == NULL) return;
    ui_page_t *page = ui_shell_find_page(shell, UI_PAGE_TYPE_OFFLINE_MENU);
    if (page != NULL)
        ui_shell_show_popup(shell, page);
}
