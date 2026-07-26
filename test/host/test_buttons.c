// The reset button's hold thresholds (SPEC.md 3.12). Small surface, but the
// consequence of getting it wrong is asymmetric: a boundary that fires one
// step too eagerly turns "restart" into "wipe everything the user configured".
// So the boundaries are pinned exactly, including the millisecond either side.
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "led_state.h"   // the real one, for led_color_t

int stub_gpio_level = 1; // idle = high (active-low button)

// Stubs for what buttons.c drives.
static led_color_t g_led_primary;
static unsigned char g_led_brightness;
static int g_led_calls;
esp_err_t led_state_set_dual(led_color_t p, led_color_t s, uint8_t b)
{ (void)s; g_led_primary = p; g_led_brightness = b; g_led_calls++; return ESP_OK; }
esp_err_t led_state_off(void) { return ESP_OK; }
void schedule_task_force_check(void) {}
void factory_reset_perform(void) { for (;;) {} }
esp_err_t factory_reset_erase(void) { return ESP_OK; }

#include "buttons.c"

static int g_fail;

static void check(const char *name, bool ok, const char *detail)
{
    printf("%s %-52s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) g_fail++;
}

static const char *armed_name(armed_t a)
{
    switch (a) {
    case ARMED_NONE:    return "none";
    case ARMED_REBOOT:  return "reboot";
    case ARMED_FACTORY: return "factory";
    }
    return "?";
}

static void expect_armed(uint32_t held_ms, armed_t want)
{
    armed_t got = armed_for_hold(held_ms);
    char name[80], detail[64];
    snprintf(name, sizeof(name), "held %5ums -> %s", (unsigned)held_ms, armed_name(want));
    snprintf(detail, sizeof(detail), "got %s", armed_name(got));
    check(name, got == want, detail);
}

int main(void)
{
    printf("\n== hold thresholds ==\n");
    expect_armed(0,     ARMED_NONE);
    expect_armed(500,   ARMED_NONE);
    expect_armed(2999,  ARMED_NONE);      // a tap, or a slip, does nothing
    expect_armed(3000,  ARMED_REBOOT);    // exactly 3s arms restart
    expect_armed(5000,  ARMED_REBOOT);
    expect_armed(9999,  ARMED_REBOOT);    // one ms short of a wipe
    expect_armed(10000, ARMED_FACTORY);   // exactly 10s arms factory reset
    expect_armed(60000, ARMED_FACTORY);   // and stays there, however long

    printf("\n== the dangerous boundary ==\n");
    // Nothing between 3s and 10s may resolve to a factory reset - that's the
    // whole safety margin between "restart it" and "lose everything".
    int wrong = 0;
    for (uint32_t t = REBOOT_HOLD_MS; t < FACTORY_HOLD_MS; t += 50) {
        if (armed_for_hold(t) != ARMED_REBOOT) {
            wrong++;
        }
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "%d of %d samples wrong", wrong,
             (int)((FACTORY_HOLD_MS - REBOOT_HOLD_MS) / 50));
    check("every 50ms tick in [3s,10s) arms restart only", wrong == 0, detail);

    // And nothing below 3s arms anything at all.
    wrong = 0;
    for (uint32_t t = 0; t < REBOOT_HOLD_MS; t += 50) {
        if (armed_for_hold(t) != ARMED_NONE) {
            wrong++;
        }
    }
    snprintf(detail, sizeof(detail), "%d wrong", wrong);
    check("every tick below 3s arms nothing", wrong == 0, detail);

    printf("\n== armed feedback ==\n");
    // The user can't time a 10-second hold by feel, so the LEDs have to say
    // which action is armed - and the two must be visibly different.
    g_led_calls = 0;
    show_armed(ARMED_REBOOT);
    led_color_t reboot_color = g_led_primary;
    show_armed(ARMED_FACTORY);
    led_color_t factory_color = g_led_primary;
    snprintf(detail, sizeof(detail), "reboot=(%u,%u,%u) factory=(%u,%u,%u)",
             reboot_color.r, reboot_color.g, reboot_color.b,
             factory_color.r, factory_color.g, factory_color.b);
    check("restart and factory-reset show different colours",
          memcmp(&reboot_color, &factory_color, sizeof(led_color_t)) != 0, detail);
    check("armed feedback is actually lit", g_led_brightness > 0, "");

    g_led_calls = 0;
    show_armed(ARMED_NONE);
    check("no feedback before 3s (LEDs left to the schedule)", g_led_calls == 0, "");

    printf("\n== button polarity ==\n");
    stub_gpio_level = 0;
    check("active-low: level 0 reads as pressed", button_is_pressed(), "");
    stub_gpio_level = 1;
    check("active-low: level 1 reads as released", !button_is_pressed(), "");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all passed", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
