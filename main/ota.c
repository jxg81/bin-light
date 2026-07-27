#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "schedule.h"

static const char *TAG = "ota";

#define MANIFEST_BUF_SIZE  1024
#define HTTP_TIMEOUT_MS    10000
// The image is streamed to flash, so this only bounds one read at a time.
#define OTA_RX_BUFFER      4096

static SemaphoreHandle_t s_mutex;
static ota_state_t s_state = OTA_STATE_IDLE;
static char s_message[96] = "";
static char s_pending_url[256];
// Whether the flashing task reboots itself once the image is written. Set per
// call by ota_start(); see the two callers' opposite needs in ota.h.
static bool s_restart_when_done;

static void set_state(ota_state_t state, const char *fmt, ...)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = state;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_message, sizeof(s_message), fmt, args);
    va_end(args);
    xSemaphoreGive(s_mutex);
}

const char *ota_running_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return (desc != NULL && desc->version[0] != '\0') ? desc->version : "unknown";
}

// ------------------------------------------------------------- manifest --

typedef struct {
    char   *buf;
    size_t  capacity;
    size_t  len;
    bool    overflow;
} manifest_buf_t;

static esp_err_t manifest_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        manifest_buf_t *mb = (manifest_buf_t *)evt->user_data;
        if (mb != NULL && evt->data_len > 0) {
            if (mb->len + (size_t)evt->data_len + 1 > mb->capacity) {
                mb->overflow = true;
            } else {
                memcpy(mb->buf + mb->len, evt->data, evt->data_len);
                mb->len += (size_t)evt->data_len;
                mb->buf[mb->len] = '\0';
            }
        }
    }
    return ESP_OK;
}

// Minimal string-field extractor. cJSON is available (waste_api uses it), but
// the manifest is three flat string fields written by us - pulling in a parse
// tree for that is more moving parts than the job needs, and this cannot
// recurse or allocate.
static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t o = 0;
    while (*p != '\0' && *p != '"' && o + 1 < out_size) {
        if (*p == '\\' && p[1] != '\0') {
            p++; // keep escaped characters literal; no escapes are expected
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

esp_err_t ota_check(ota_manifest_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char *buf = malloc(MANIFEST_BUF_SIZE);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    buf[0] = '\0';
    manifest_buf_t mb = { .buf = buf, .capacity = MANIFEST_BUF_SIZE };

    esp_http_client_config_t config = {
        .url = CONFIG_BINLIGHT_OTA_MANIFEST_URL,
        .event_handler = manifest_event_handler,
        .user_data = &mb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(buf);
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || mb.overflow) {
        ESP_LOGW(TAG, "manifest fetch failed (err=%s status=%d overflow=%d)",
                 esp_err_to_name(err), status, mb.overflow);
        free(buf);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    bool ok = json_get_string(buf, "version", out->version, sizeof(out->version)) &&
              json_get_string(buf, "url", out->url, sizeof(out->url));
    json_get_string(buf, "notes", out->notes, sizeof(out->notes)); // optional
    free(buf);

    if (!ok) {
        ESP_LOGW(TAG, "manifest missing 'version' or 'url'");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Any difference counts, in either direction - see ota.h on why this is
    // not a "newer than" comparison.
    out->available = (strcmp(out->version, ota_running_version()) != 0);
    ESP_LOGI(TAG, "manifest: version=%s running=%s available=%d",
             out->version, ota_running_version(), out->available);
    return ESP_OK;
}

// ---------------------------------------------------------------- apply --

static void ota_task_fn(void *arg)
{
    ESP_LOGW(TAG, "starting OTA from %s", s_pending_url);
    set_state(OTA_STATE_RUNNING, "starting");

    esp_http_client_config_t http_config = {
        .url = s_pending_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        // A GitHub release download is a 302 to a *different host* -
        // release-assets.githubusercontent.com as of 2026-07 (it was
        // objects.githubusercontent.com; the docs and most examples still say
        // so, which is why this is written down rather than assumed). The
        // signed URL it hands back is ~900 characters and carries a JWT that
        // expires in about an hour, so it must be followed promptly and
        // cannot be cached or hardcoded. esp_http_client follows redirects by
        // default; this must not be disabled.
        //
        // **Both buffer sizes must be set, and the TX one is why.** After the
        // redirect, esp_http_client composes the request line as
        // "GET <path>?<query> HTTP/1.1" into a buffer of buffer_size_tx and
        // fails with a bare `HTTP_CLIENT: Out of buffer` if it doesn't fit
        // (esp_http_client.c, http_client_prepare_first_line). The default is
        // CONFIG_ESP_HTTP_CLIENT_MAX_TX_BUFFER_SIZE = 512, and almost all of
        // that ~900-character URL is *query string* - the signature. So every
        // GitHub-hosted OTA fails at the redirect, with an error that names
        // neither the URL nor the buffer. See SPEC.md 6 bug 23.
        //
        // 4096 is deliberate headroom, not a measured minimum: the URL length
        // is GitHub's to change and this failure is invisible until an update
        // is actually published. RX only needs to hold the 302's headers (the
        // Location line being the long one), so 2048 is ample there.
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK || handle == NULL) {
        set_state(OTA_STATE_FAILED, "couldn't start: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    int last_pct = -1;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(handle);
        int pct = (total > 0) ? (done * 100 / total) : 0;
        if (pct != last_pct && pct % 5 == 0) {
            last_pct = pct;
            set_state(OTA_STATE_RUNNING, "downloading %d%%", pct);
            ESP_LOGI(TAG, "OTA %d%% (%d/%d bytes)", pct, done, total);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        set_state(OTA_STATE_FAILED, "download failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        set_state(OTA_STATE_FAILED, "incomplete image - connection dropped");
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        // ESP_ERR_OTA_VALIDATE_FAILED means the image failed its own integrity
        // check - a corrupt or non-ESP-IDF file rather than a network problem.
        set_state(OTA_STATE_FAILED, "image rejected: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "OTA complete - the new image boots on next restart");

    if (s_restart_when_done) {
        // Someone is watching this happen, so finish the job rather than
        // leaving them to find a second button. The delay is for the browser,
        // not the flash: the page polls every 3s, and rebooting the instant
        // the write finishes means the "installed" state is never once
        // rendered and the user sees only a dead connection.
        set_state(OTA_STATE_SUCCESS, "installed - restarting now");
        ESP_LOGW(TAG, "restarting into the new firmware");
        vTaskDelay(pdMS_TO_TICKS(4000));
        esp_restart();
    }

    set_state(OTA_STATE_SUCCESS, "installed - restart to run it");
    vTaskDelete(NULL);
}

esp_err_t ota_start(const char *url, bool restart_when_done)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (ota_get_state() == OTA_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_pending_url, sizeof(s_pending_url), "%s", url);
    s_restart_when_done = restart_when_done;
    set_state(OTA_STATE_RUNNING, "starting");

    // Generous stack: TLS plus the OTA machinery on one task.
    BaseType_t ok = xTaskCreate(ota_task_fn, "ota_task", 8192, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        set_state(OTA_STATE_FAILED, "couldn't start the update task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

ota_state_t ota_get_state(void)
{
    if (s_mutex == NULL) {
        return OTA_STATE_IDLE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ota_state_t state = s_state;
    xSemaphoreGive(s_mutex);
    return state;
}

const char *ota_get_message(void)
{
    return s_message;
}

// ------------------------------------------------------- automatic updates --

// Stored as its own small NVS key rather than a field in schedule_t. Adding a
// field there changes sizeof(schedule_t), which fails schedule_init()'s size
// check and resets every configured device back to defaults - unacceptable now
// that real ones are deployed. A separate key needs no migration, and
// factory_reset already erases the whole "binlight" namespace, so it is still
// covered by a reset.
#define OTA_NVS_NAMESPACE  "binlight"
#define OTA_NVS_AUTO_KEY   "ota_auto"

#define AUTO_FIRST_CHECK_MS (2UL * 60 * 1000)        // let Wi-Fi and SNTP settle
#define AUTO_CHECK_INTERVAL_MS (24UL * 60 * 60 * 1000)
#define AUTO_REBOOT_POLL_MS (60UL * 1000)

bool ota_auto_update_enabled(void)
{
    nvs_handle_t handle;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return true; // default on - see ota.h
    }
    uint8_t value = 1;
    esp_err_t err = nvs_get_u8(handle, OTA_NVS_AUTO_KEY, &value);
    nvs_close(handle);
    return (err == ESP_OK) ? (value != 0) : true;
}

esp_err_t ota_set_auto_update(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, OTA_NVS_AUTO_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "automatic updates %s", enabled ? "enabled" : "disabled");
    return err;
}

static void auto_task_fn(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(AUTO_FIRST_CHECK_MS));

    for (;;) {
        if (ota_auto_update_enabled() && ota_get_state() != OTA_STATE_RUNNING) {
            ota_manifest_t m;
            if (ota_check(&m) == ESP_OK && m.available) {
                ESP_LOGW(TAG, "auto-update: installing %s", m.version);
                // false: this path does its own restart below, once the
                // light is off, so it must not be pre-empted here.
                if (ota_start(m.url, false) == ESP_OK) {
                    while (ota_get_state() == OTA_STATE_RUNNING) {
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (ota_get_state() == OTA_STATE_SUCCESS) {
                        // Never restart mid-display: the light being on is the
                        // one moment the device is actually doing its job, and
                        // a reboot would drop it for the ~10s of a boot cycle
                        // plus the self-test.
                        while (schedule_light_is_on()) {
                            ESP_LOGI(TAG, "auto-update: installed, waiting for the light to go off");
                            vTaskDelay(pdMS_TO_TICKS(AUTO_REBOOT_POLL_MS));
                        }
                        ESP_LOGW(TAG, "auto-update: restarting into the new firmware");
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_restart();
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(AUTO_CHECK_INTERVAL_MS));
    }
}

esp_err_t ota_auto_task_start(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "automatic updates are %s", ota_auto_update_enabled() ? "on" : "off");
    BaseType_t ok = xTaskCreate(auto_task_fn, "ota_auto", 6144, NULL, tskIDLE_PRIORITY + 1, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

// How long a new image gets to finish starting up before it is presumed dead.
//
// Ten minutes is chosen against the *false positive*, not the true one. A
// healthy boot reaches ota_mark_valid() in seconds, so any value above about a
// minute catches a genuine hang. The risk being sized for is the opposite one:
// a good update landing while the router happens to be rebooting, where the
// device correctly waits in AutoAP and would be rolled back for it. Household
// outages are minutes, not tens of minutes.
#define ROLLBACK_WATCHDOG_MS (10 * 60 * 1000)

// A dedicated task rather than a FreeRTOS software timer, on purpose.
//
// The obvious implementation is a one-shot timer, and that is what this was
// first written as. The problem is where a timer callback runs: the timer
// service task, whose stack is CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH = 2048
// bytes. esp_ota_mark_app_invalid_rollback_and_reboot() writes the otadata
// partition, and running flash operations on a 2KB stack is at best
// uncomfortably tight.
//
// What makes that worth avoiding is not the crash itself but how it would
// read: a stack overflow panics, the panic reboots, and the reboot rolls the
// image back - because it is still PENDING_VERIFY. The device would recover
// and the test would look like a pass, while proving nothing about the
// watchdog. A wrong mechanism producing the right outcome is the one failure
// that hides itself.
//
// A task also expresses the logic better: wait for either the deadline or
// notification that startup finished, whichever comes first.
#define ROLLBACK_TASK_STACK 4096

static TaskHandle_t s_rollback_task;

static void rollback_watchdog_task(void *arg)
{
    (void)arg;

    // Returns non-zero if ota_mark_valid() notified us, 0 if the wait expired.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ROLLBACK_WATCHDOG_MS)) != 0) {
        s_rollback_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGE(TAG, "this image never finished starting up after %d minutes - rolling back",
             ROLLBACK_WATCHDOG_MS / 60000);

    // Marks the running image invalid and reboots, so the bootloader takes the
    // previous slot. Only returns if it *couldn't* - e.g. there is no earlier
    // image to fall back to. Carry on rather than restarting blindly in that
    // case: a reboot that changes nothing would just loop.
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback refused (%s) - staying on this image", esp_err_to_name(err));
    s_rollback_task = NULL;
    vTaskDelete(NULL);
}

void ota_rollback_watchdog_start(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; // ordinary boot of a confirmed image - nothing to guard
    }

    if (xTaskCreate(rollback_watchdog_task, "ota_rollback", ROLLBACK_TASK_STACK, NULL,
                    tskIDLE_PRIORITY + 1, &s_rollback_task) != pdPASS) {
        // Not fatal, but say so plainly: without this the only protection left
        // is the image crashing on its own.
        ESP_LOGE(TAG, "could not arm the rollback watchdog - a hang would not be recovered");
        s_rollback_task = NULL;
        return;
    }
    ESP_LOGW(TAG, "unverified image: rolling back unless startup completes within %d minutes",
             ROLLBACK_WATCHDOG_MS / 60000);
}

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; // an ordinary boot of an already-confirmed image
    }

    // Disarm first. Confirming the image and then leaving a watchdog running
    // that would roll it back anyway is the one ordering that must not happen.
    if (s_rollback_task != NULL) {
        xTaskNotifyGive(s_rollback_task);
    }

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "new firmware confirmed healthy - rollback cancelled");
    } else {
        ESP_LOGE(TAG, "failed to confirm the new firmware; it will roll back on reboot");
    }
}
