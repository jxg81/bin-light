#include "led_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

#define LED_COUNT 2
#define SELF_TEST_STEP_MS 500

static const char *TAG = "led_state";

// The 4 colours the web UI's rotation presets also use (see COLOR_PRESETS in
// web_server.c) - duplicated here rather than shared, since led_state.c is a
// low-level hardware driver and shouldn't depend on the HTTP layer for 4 RGB
// literals.
static const led_color_t SELF_TEST_COLORS[] = {
    {255, 0,   0},   // Red
    {0,   255, 0},   // Green
    {255, 150, 0},   // Yellow (green pulled down - see COLOR_PRESETS in web_server.c)
    {128, 0,   128}, // Purple
};
#define SELF_TEST_COLOR_COUNT (sizeof(SELF_TEST_COLORS) / sizeof(SELF_TEST_COLORS[0]))

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_mutex;
static led_color_t s_current_color;
static uint8_t s_current_brightness;

esp_err_t led_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_BINLIGHT_LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        // This specific LED batch was confirmed (by observed colour swap on
        // real hardware) to expect RGB byte order on the wire, not the
        // datasheet-standard GRB. Do not "correct" this back to GRB.
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init LED strip: %s", esp_err_to_name(err));
        return err;
    }

    return led_state_off();
}

esp_err_t led_state_set_dual(led_color_t primary, led_color_t secondary, uint8_t brightness)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint8_t r0 = (uint16_t)primary.r * brightness / 255;
    uint8_t g0 = (uint16_t)primary.g * brightness / 255;
    uint8_t b0 = (uint16_t)primary.b * brightness / 255;
    uint8_t r1 = (uint16_t)secondary.r * brightness / 255;
    uint8_t g1 = (uint16_t)secondary.g * brightness / 255;
    uint8_t b1 = (uint16_t)secondary.b * brightness / 255;

    esp_err_t err = led_strip_set_pixel(s_strip, 0, r0, g0, b0);
    if (err == ESP_OK) {
        err = led_strip_set_pixel(s_strip, 1, r1, g1, b1);
    }
    if (err == ESP_OK) {
        err = led_strip_refresh(s_strip);
    }
    if (err == ESP_OK) {
        s_current_color = primary;
        s_current_brightness = brightness;
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t led_state_set(led_color_t color, uint8_t brightness)
{
    return led_state_set_dual(color, color, brightness);
}

esp_err_t led_state_off(void)
{
    return led_state_set_dual((led_color_t){0, 0, 0}, (led_color_t){0, 0, 0}, 0);
}

led_color_t led_state_get_current(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_color_t color = s_current_color;
    xSemaphoreGive(s_mutex);
    return color;
}

// --- breathing indicator (SPEC.md 3.4) --------------------------------------

#define BREATHE_TICK_MS    40
#define BREATHE_PERIOD_MS  3000  // one full up-and-down cycle
#define BREATHE_MIN        8     // never fully dark - "off" would read as idle
#define BREATHE_MAX        160   // calm, not glaring

static TaskHandle_t s_breathe_task;
static volatile bool s_breathe_stop_requested;
static led_color_t s_breathe_color;

static void breathe_task_fn(void *arg)
{
    const int steps = BREATHE_PERIOD_MS / BREATHE_TICK_MS;
    const int half = steps / 2;
    int step = 0;

    while (!s_breathe_stop_requested) {
        // Triangle ramp: up for the first half of the period, down for the
        // second. A triangle reads as smooth at 40ms ticks without pulling in
        // floating-point or a sine table.
        int pos = (step < half) ? step : (steps - step);
        uint8_t b = (uint8_t)(BREATHE_MIN + (BREATHE_MAX - BREATHE_MIN) * pos / half);
        led_state_set_dual(s_breathe_color, s_breathe_color, b);
        step = (step + 1) % steps;
        vTaskDelay(pdMS_TO_TICKS(BREATHE_TICK_MS));
    }
    led_state_off();
    s_breathe_task = NULL;
    vTaskDelete(NULL);
}

void led_state_breathe_start(led_color_t color)
{
    if (s_breathe_task != NULL) {
        return;
    }
    s_breathe_color = color;
    s_breathe_stop_requested = false;
    if (xTaskCreate(breathe_task_fn, "led_breathe", 2048, NULL, tskIDLE_PRIORITY + 2, &s_breathe_task) != pdPASS) {
        ESP_LOGW(TAG, "failed to start breathe task");
        s_breathe_task = NULL;
    }
}

void led_state_breathe_stop(void)
{
    if (s_breathe_task == NULL) {
        return;
    }
    s_breathe_stop_requested = true;
    // The task exits within one tick; wait for it so the caller can safely
    // drive the LEDs the moment this returns.
    while (s_breathe_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void led_state_run_self_test(void)
{
    static const led_color_t off = {0, 0, 0};

    for (size_t i = 0; i < SELF_TEST_COLOR_COUNT; i++) {
        led_state_set_dual(SELF_TEST_COLORS[i], off, 255);
        vTaskDelay(pdMS_TO_TICKS(SELF_TEST_STEP_MS));
    }
    for (size_t i = 0; i < SELF_TEST_COLOR_COUNT; i++) {
        led_state_set_dual(off, SELF_TEST_COLORS[i], 255);
        vTaskDelay(pdMS_TO_TICKS(SELF_TEST_STEP_MS));
    }
    for (size_t i = 0; i < SELF_TEST_COLOR_COUNT; i++) {
        led_state_set_dual(SELF_TEST_COLORS[i], SELF_TEST_COLORS[i], 255);
        vTaskDelay(pdMS_TO_TICKS(SELF_TEST_STEP_MS));
    }
    led_state_off();
}
