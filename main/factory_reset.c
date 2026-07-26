#include "factory_reset.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "factory_reset";

// Every NVS namespace this firmware writes to. Listed explicitly rather than
// erasing the whole partition, so a reset stays a *settings* reset: it can't
// take out anything the system stores for itself.
//
// Both entries matter. "binlight" holds the schedule, the council/API config
// and its next-collection cache, and the Wi-Fi credentials; "binlight_cfg"
// holds only the timezone - a split that dates from settings.c being written
// separately. Erasing just the obvious one would leave a device claiming to
// be factory-fresh while still sitting in whatever timezone it was given.
static const char *const NVS_NAMESPACES[] = {
    "binlight",      // schedule_v5, waste_api_v3, waste_cache_v1, wifi_cred_v1
    "binlight_cfg",  // tz
};

static esp_err_t erase_namespace(const char *ns)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; // never written to - already in the state we want
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not open namespace \"%s\": %s", ns, esp_err_to_name(err));
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to erase namespace \"%s\": %s", ns, esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "erased namespace \"%s\"", ns);
    }
    return err;
}

esp_err_t factory_reset_erase(void)
{
    ESP_LOGW(TAG, "factory reset: erasing all persisted settings");

    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < sizeof(NVS_NAMESPACES) / sizeof(NVS_NAMESPACES[0]); i++) {
        esp_err_t err = erase_namespace(NVS_NAMESPACES[i]);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }

    // The Wi-Fi driver keeps its own copy of the last SSID/password in its
    // own NVS namespace, independent of ours. Without this the credentials
    // are gone from our config but still sitting in the driver's store -
    // which is not what "all configuration will be lost" promises, even
    // though our own boot path would no longer use them.
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_restore failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }

    return first_err;
}

void factory_reset_perform(void)
{
    factory_reset_erase();
    ESP_LOGW(TAG, "factory reset complete, restarting");
    // A short settle so the log lines actually leave the UART before reset.
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    for (;;) { } // esp_restart() does not return; satisfies noreturn
}
