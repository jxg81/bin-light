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
    {255, 255, 0},   // Yellow
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
