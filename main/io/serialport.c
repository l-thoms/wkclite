#include "serialport.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io/power_key.h"
#include "ui/shell.h"
#include "ui/ui_common.h"

#define TAG "WKC_SERIAL"

// Key input is read from the console UART (UART0), the same port the log
// output uses. Only the RX side is touched, so logging keeps working.
#define SERIALPORT_UART UART_NUM_0
#define SERIALPORT_RX_BUFFER 1024
#define SERIALPORT_POLL_MS 20
// Minimum time between two injected keys. Holding a key on the PC repeats the
// character every few milliseconds; those repeats are dropped instead of being
// queued, so the UI stops as soon as the key is released.
#define SERIALPORT_KEY_INTERVAL_MS 100
// No new character for this long is treated as "key released"
#define SERIALPORT_RELEASE_MS 200
// Pseudo key code for the GPIO0 (power) key
#define SERIALPORT_KEY_GPIO0 0x100
// Three 'z' presses closer than this simulate one GPIO0 long press
#define SERIALPORT_GPIO0_LONG_PRESSES 3
#define SERIALPORT_GPIO0_INTERVAL_MS 500

static int gpio0_presses = 0;
static int64_t gpio0_last_time = 0;
static bool gpio0_consumed = false;

// Accumulates 'z' presses into a short or a long GPIO0 event
static void serialport_gpio0_press(int64_t now)
{
    if (now - gpio0_last_time > SERIALPORT_GPIO0_INTERVAL_MS)
    {
        gpio0_presses = 0;
        gpio0_consumed = false;
    }
    gpio0_last_time = now;
    if (gpio0_consumed)
        return;
    gpio0_presses += 1;
    if (gpio0_presses >= SERIALPORT_GPIO0_LONG_PRESSES)
    {
        // Three presses are one long press; further presses do not count
        gpio0_consumed = true;
        gpio0_presses = 0;
        power_key_event(true);
    }
}

// Emits the short presses once the gesture has ended
static void serialport_gpio0_poll(int64_t now)
{
    if (gpio0_presses == 0 || gpio0_consumed)
        return;
    if (now - gpio0_last_time <= SERIALPORT_GPIO0_INTERVAL_MS)
        return;
    int presses = gpio0_presses;
    gpio0_presses = 0;
    for (int i = 0; i < presses; i++)
        power_key_event(false);
}

// Translates one received character into a key code, 0 when it is not a key
static int serialport_map_key(uint8_t c)
{
    switch (c)
    {
        case 'w':
        case 'W':
            return UI_KEY_CODE_UP;
        case 'a':
        case 'A':
            return UI_KEY_CODE_LEFT;
        case 's':
        case 'S':
            return UI_KEY_CODE_DOWN;
        case 'd':
        case 'D':
            return UI_KEY_CODE_RIGHT;
        case ' ':
            return UI_KEY_CODE_OK;
        case 'q':
        case 'Q':
            return UI_KEY_CODE_EXIT;
        case 'e':
        case 'E':
            return UI_KEY_CODE_MENU;
        case 'z':
        case 'Z':
            return SERIALPORT_KEY_GPIO0;
        default:
            // Ignore line endings and any other character
            return 0;
    }
}

static void serialport_inject(int key)
{
    ui_shell_t *shell = ui_shell_get_current();
    if (shell == NULL)
        return;
    ui_shell_send_key(shell, key);
}

static void serialport_task(void *args)
{
    uint8_t c;
    int64_t now, last_rx = 0, last_inject = 0;
    int pending = 0;

    while (true)
    {
        now = esp_timer_get_time() / 1000;

        // Drain the receive buffer. The first key of a burst goes out right
        // away; every following repeat only replaces `pending`, so a held key
        // can never build up a queue of key presses.
        while (uart_read_bytes(SERIALPORT_UART, &c, 1, 0) == 1)
        {
            int key = serialport_map_key(c);
            if (key == 0)
                continue;

            last_rx = now;
            if (key == SERIALPORT_KEY_GPIO0)
            {
                // GPIO0 is simulated by a sequence of 'z' presses
                serialport_gpio0_press(now);
                continue;
            }
            if (now - last_inject >= SERIALPORT_KEY_INTERVAL_MS)
            {
                // Key just pressed (or the rate limit allows it): deliver now
                serialport_inject(key);
                pending = 0;
                last_inject = now;
            }
            else
            {
                // Key is still held: keep only the newest repeat
                pending = key;
            }
        }

        now = esp_timer_get_time() / 1000;

        // Deliver the key that is still held, and the last one at release,
        // once the rate limit has expired (or the stream stopped)
        if (pending != 0 &&
            (now - last_inject >= SERIALPORT_KEY_INTERVAL_MS ||
             now - last_rx >= SERIALPORT_RELEASE_MS))
        {
            serialport_inject(pending);
            pending = 0;
            last_inject = now;
        }

        serialport_gpio0_poll(esp_timer_get_time() / 1000);

        vTaskDelay(pdMS_TO_TICKS(SERIALPORT_POLL_MS));
    }
}

void serialport_init()
{
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;

    // The console already configured UART0 (115200 8N1); install the driver
    // only when it is not there yet, and never reconfigure the port so the log
    // output is not disturbed.
    if (!uart_is_driver_installed(SERIALPORT_UART))
    {
        ESP_ERROR_CHECK(uart_driver_install(SERIALPORT_UART,
                                            SERIALPORT_RX_BUFFER, 0, 0, NULL, 0));
    }
    uart_flush_input(SERIALPORT_UART);

    xTaskCreatePinnedToCore(serialport_task, "serialport", 3072, NULL, 2, NULL, 1);
}
