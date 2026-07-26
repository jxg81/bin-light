#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "buttons.h"
#include "led_state.h"
#include "ota.h"
#include "schedule.h"
#include "settings.h"
#include "time_sync.h"
#include "waste_api.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "bin_light";

static esp_err_t nvs_init_with_erase_retry(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_hostname_set("binlight");
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_instance_name_set("Bin Light");
    if (err != ESP_OK) {
        return err;
    }
    return mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init_with_erase_retry());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(led_state_init());
    led_state_run_self_test();
    led_state_set((led_color_t){0, 0, 40}, 255); // dim blue = "booting"

    ESP_ERROR_CHECK(settings_init());

    if (wifi_manager_start() != ESP_OK) {
        ESP_LOGW(TAG, "starting without a confirmed Wi-Fi connection, will keep retrying in the background");
    }
    if (mdns_start() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS failed to start, device will only be reachable by IP");
    }
    if (time_sync_start() != ESP_OK) {
        ESP_LOGW(TAG, "starting without a confirmed time sync, schedule will activate once time becomes valid");
    }

    ESP_ERROR_CHECK(schedule_init());
    ESP_ERROR_CHECK(waste_api_init());
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(schedule_task_start());
    ESP_ERROR_CHECK(waste_api_task_start());
    ESP_ERROR_CHECK(buttons_start());

    // Everything is up and the device has proved it can serve pages, so a
    // freshly-OTA'd image is healthy. Until this runs, the bootloader will
    // roll back to the previous image on the next reboot - which is what
    // saves a physical visit if an update ever bricks the network path.
    ota_mark_valid();

    ESP_LOGI(TAG, "bin light ready (firmware %s)", ota_running_version());
}
