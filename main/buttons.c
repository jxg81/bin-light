#include "buttons.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "factory_reset.h"
#include "led_state.h"
#include "schedule.h"

static const char *TAG = "buttons";

#define POLL_MS            50
// Two consecutive agreeing reads before a change is believed. At a 50ms poll
// that's 100ms of settling - far longer than any switch bounce, and still
// imperceptible to the person pressing.
#define DEBOUNCE_SAMPLES   2

#define REBOOT_HOLD_MS     (3UL * 1000)
#define FACTORY_HOLD_MS    (10UL * 1000)

// Feedback while held. Deliberately not the bin palette: these are device
// states, not collections. Blue reads as benign, red as destructive, and both
// are steady rather than flashing so it's obvious the device is waiting on
// *you* rather than doing something.
#define ARMED_BRIGHTNESS   120
static const led_color_t COLOR_REBOOT_ARMED  = {0, 40, 255};
static const led_color_t COLOR_FACTORY_ARMED = {255, 0, 0};

typedef enum {
    ARMED_NONE = 0,
    ARMED_REBOOT,
    ARMED_FACTORY,
} armed_t;

static bool button_is_pressed(void)
{
    int level = gpio_get_level(CONFIG_BINLIGHT_RESET_BUTTON_GPIO);
#if CONFIG_BINLIGHT_RESET_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

static armed_t armed_for_hold(uint32_t held_ms)
{
    if (held_ms >= FACTORY_HOLD_MS) {
        return ARMED_FACTORY;
    }
    if (held_ms >= REBOOT_HOLD_MS) {
        return ARMED_REBOOT;
    }
    return ARMED_NONE;
}

static void show_armed(armed_t armed)
{
    switch (armed) {
    case ARMED_REBOOT:
        led_state_set_dual(COLOR_REBOOT_ARMED, COLOR_REBOOT_ARMED, ARMED_BRIGHTNESS);
        break;
    case ARMED_FACTORY:
        led_state_set_dual(COLOR_FACTORY_ARMED, COLOR_FACTORY_ARMED, ARMED_BRIGHTNESS);
        break;
    case ARMED_NONE:
        break;
    }
}

static void buttons_task_fn(void *arg)
{
    bool stable_pressed = false;
    int agree_count = 0;
    uint32_t held_ms = 0;
    armed_t shown = ARMED_NONE;
    bool led_taken = false;

    for (;;) {
        bool raw = button_is_pressed();

        if (raw == stable_pressed) {
            agree_count = 0;
        } else if (++agree_count >= DEBOUNCE_SAMPLES) {
            stable_pressed = raw;
            agree_count = 0;

            if (stable_pressed) {
                held_ms = 0;
                shown = ARMED_NONE;
                // Logged so wiring can be confirmed over serial immediately,
                // rather than inferred from the LEDs 3 seconds later - useful
                // when the "button" is a jumper wire on a breadboard.
                ESP_LOGI(TAG, "reset button pressed (GPIO %d) - hold 3s to restart, 10s to factory reset",
                         CONFIG_BINLIGHT_RESET_BUTTON_GPIO);
            } else {
                // Released - act on however long it was held.
                armed_t action = armed_for_hold(held_ms);
                if (led_taken) {
                    // Hand the LEDs back before acting, so a cancelled press
                    // doesn't leave them stuck on the armed colour.
                    led_taken = false;
                    schedule_task_force_check();
                }
                switch (action) {
                case ARMED_FACTORY:
                    ESP_LOGW(TAG, "reset button held %lums - factory reset", (unsigned long)held_ms);
                    factory_reset_perform(); // does not return
                    break;
                case ARMED_REBOOT:
                    ESP_LOGW(TAG, "reset button held %lums - restarting", (unsigned long)held_ms);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                case ARMED_NONE:
                    ESP_LOGI(TAG, "reset button released after %lums - no action (hold 3s to restart)",
                             (unsigned long)held_ms);
                    break;
                }
            }
        }

        if (stable_pressed) {
            held_ms += POLL_MS;
            armed_t armed = armed_for_hold(held_ms);
            if (armed != shown) {
                shown = armed;
                if (armed != ARMED_NONE) {
                    led_taken = true;
                    show_armed(armed);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t buttons_start(void)
{
    if (CONFIG_BINLIGHT_RESET_BUTTON_GPIO < 0) {
        ESP_LOGI(TAG, "reset button disabled by configuration");
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_BINLIGHT_RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_BINLIGHT_RESET_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure GPIO %d: %s",
                 CONFIG_BINLIGHT_RESET_BUTTON_GPIO, esp_err_to_name(err));
        return err;
    }

    // Polling rather than an interrupt: the whole job is measuring a
    // multi-second hold, so a 50ms tick is plenty and it keeps all the timing
    // in one readable place instead of split across an ISR and a task.
    BaseType_t ok = xTaskCreate(buttons_task_fn, "buttons", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "reset button on GPIO %d (hold 3s restart, 10s factory reset)",
             CONFIG_BINLIGHT_RESET_BUTTON_GPIO);
    return ESP_OK;
}
