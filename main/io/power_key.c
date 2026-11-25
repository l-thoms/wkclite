#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "peripherals/lock.h"
#include "peripherals/battery_calibration.h"
#include "io_extend.h"
#include "display/display_control.h"
#include "ui/shell.h"
#include "ui/page.h"
#include "ui/home.h"
#include "ui/menu.h"
#include "ui/offline_menu.h"
#include "power_key.h"

// Time the key has to be held down to count as a long press
#define POWER_KEY_LONG_PRESS_US 1000000
// Upper bound for waiting until the key that requested standby is released
#define POWER_KEY_RELEASE_TIMEOUT_US 5000000

static ui_shell_t *current_shell = NULL;
static TaskHandle_t power_key_task_handle = NULL;
// Set when standby wakes up again: the press that woke the device is swallowed
static volatile bool suppress_next_event = false;

static void power_key_isr(void *args)
{
    BaseType_t woken = pdFALSE;
    if (power_key_task_handle != NULL)
        vTaskNotifyGiveFromISR(power_key_task_handle, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

// Enters light sleep until the key is pressed again
void power_key_standby()
{
    // The entry is triggered while the key is still held down; the level
    // triggered wakeup source would fire immediately, so wait for the release
    int64_t start = esp_timer_get_time();
    while (gpio_read(GPIO_NUM_0) == 0 &&
           esp_timer_get_time() - start < POWER_KEY_RELEASE_TIMEOUT_US)
        vTaskDelay(pdMS_TO_TICKS(10));

    suppress_next_event = true;
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_light_sleep_start();
    display_reset_power_save();
}

// Dispatches one GPIO0 event: short press or long press
void power_key_event(bool long_press)
{
    // The press that woke the device from standby only wakes it up
    if (suppress_next_event)
    {
        suppress_next_event = false;
        return;
    }
    ui_shell_t *shell = current_shell;
    // Wake the display when it is powered down
    if (display_get_power_save())
    {
        display_reset_power_save();
        return;
    }
    if (shell == NULL)
        return;

    // The head shell lock is released with a long press; a short press keeps
    // working normally so the GPIO0 menu can be used without unlocking first
    if (lock_get_state() && long_press)
    {
        // Queued to the lock task (bounded wait) so the key handling never blocks
        lock_set_from_isr(false, true, false);
        return;
    }

    // The GPIO0 menu is open: short press moves, long press activates
    if (ui_shell_get_popup(shell) != NULL)
    {
        ui_shell_send_key(shell, long_press ? UI_KEY_CODE_GPIO0_LONG :
                          UI_KEY_CODE_GPIO0_SHORT);
        return;
    }
    if (long_press)
        return;
    // Short press on the device information page returns to the home page
    ui_page_t *page = ui_shell_get_current_page(shell);
    if (page != NULL && page->type == UI_PAGE_TYPE_DEVICE_INFO)
    {
        ui_shell_show_page(shell, ui_shell_find_page(shell, UI_PAGE_TYPE_HOME));
        return;
    }
    // Short press opens the GPIO0 menu
    ui_offline_menu_show(shell);
}

static void power_key_task(void *args)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (gpio_read(GPIO_NUM_0) != 0)
            continue;
        // Wait for the release, counting the time the key is held down
        int64_t press_time = esp_timer_get_time();
        bool long_press = false;
        while (gpio_read(GPIO_NUM_0) == 0)
        {
            if (esp_timer_get_time() - press_time >= POWER_KEY_LONG_PRESS_US)
            {
                long_press = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        power_key_event(long_press);
    }
}

void power_key_set_shell(ui_shell_t *shell)
{
    current_shell = shell;
}

void power_key_init()
{
    gpio_set_intr_type(GPIO_NUM_0, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(GPIO_NUM_0, power_key_isr, NULL);
    if (power_key_task_handle == NULL)
        xTaskCreatePinnedToCore(power_key_task, "power_key", 4096, NULL, 5,
                                &power_key_task_handle, 1);
}
