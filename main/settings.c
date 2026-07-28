#include "settings.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define SETTINGS_NVS_NAMESPACE "binlight_cfg"
#define SETTINGS_TZ_KEY        "tz"

static char s_tz[SETTINGS_TZ_MAX_LEN + 1];

static void apply_tz(void)
{
    setenv("TZ", s_tz, 1);
    tzset();
}

static void set_default_tz(void)
{
    strncpy(s_tz, CONFIG_BINLIGHT_TZ_STRING, SETTINGS_TZ_MAX_LEN);
    s_tz[SETTINGS_TZ_MAX_LEN] = '\0';
}

esp_err_t settings_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(s_tz);
        esp_err_t get_err = nvs_get_str(handle, SETTINGS_TZ_KEY, s_tz, &len);
        nvs_close(handle);
        if (get_err == ESP_OK && s_tz[0] != '\0') {
            apply_tz();
            ESP_LOGI(TAG, "loaded TZ from NVS: %s", s_tz);
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "nvs_open failed (%s), using default TZ", esp_err_to_name(err));
    }

    set_default_tz();
    apply_tz();
    ESP_LOGI(TAG, "using default TZ: %s", s_tz);
    return ESP_OK;
}

const char *settings_get_tz(void)
{
    return s_tz;
}

esp_err_t settings_set_tz(const char *tz)
{
    if (tz == NULL || tz[0] == '\0' || strlen(tz) > SETTINGS_TZ_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, SETTINGS_TZ_KEY, tz);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist TZ: %s", esp_err_to_name(err));
        return err;
    }

    strncpy(s_tz, tz, SETTINGS_TZ_MAX_LEN);
    s_tz[SETTINGS_TZ_MAX_LEN] = '\0';
    apply_tz();
    ESP_LOGI(TAG, "TZ updated to: %s", s_tz);
    return ESP_OK;
}

// Only two distinct zones cover every state the council table currently
// supports (STATE_ORDER is {VIC, NSW, QLD, TAS}). The other three are listed
// so that adding South Australia (SPEC.md 3.13.2) or a WA/NT council needs no
// change here.
const char *settings_tz_for_state(const char *state)
{
    static const struct { const char *state; const char *tz; } STATE_TZ[] = {
        {"VIC", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
        {"NSW", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
        {"ACT", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
        {"TAS", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
        {"QLD", "AEST-10"},
        {"SA",  "ACST-9:30ACDT,M10.1.0/2,M4.1.0/3"},
        {"NT",  "ACST-9:30"},
        {"WA",  "AWST-8"},
    };
    if (state == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(STATE_TZ) / sizeof(STATE_TZ[0]); i++) {
        if (strcmp(STATE_TZ[i].state, state) == 0) {
            return STATE_TZ[i].tz;
        }
    }
    return NULL;
}
