#include "time_sync.h"

#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "time_sync";

#define SNTP_SYNC_TIMEOUT_MS  10000
#define SNTP_SYNC_POLL_MS     500

static bool s_time_valid = false;

esp_err_t time_sync_start(void)
{
    // Timezone is owned by settings.c (settings_init() must run before this,
    // so setenv("TZ", ...)/tzset() has already happened).

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_BINLIGHT_SNTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_SYNC_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp sync timed out, continuing without valid time for now");
        return ESP_ERR_TIMEOUT;
    }

    s_time_valid = true;
    ESP_LOGI(TAG, "time synced");
    return ESP_OK;
}

bool time_sync_is_valid(void)
{
    if (s_time_valid) {
        return true;
    }
    // Also recognize sync happening asynchronously after the initial bounded
    // wait in time_sync_start() gave up.
    if (esp_netif_sntp_sync_wait(0) == ESP_OK) {
        s_time_valid = true;
    }
    return s_time_valid;
}
