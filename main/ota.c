#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define MANIFEST_BUF_SIZE  1024
#define HTTP_TIMEOUT_MS    10000
// The image is streamed to flash, so this only bounds one read at a time.
#define OTA_RX_BUFFER      4096

static SemaphoreHandle_t s_mutex;
static ota_state_t s_state = OTA_STATE_IDLE;
static char s_message[96] = "";
static char s_pending_url[256];

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
    set_state(OTA_STATE_SUCCESS, "installed - restart to run it");
    vTaskDelete(NULL);
}

esp_err_t ota_start(const char *url)
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
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "new firmware confirmed healthy - rollback cancelled");
    } else {
        ESP_LOGE(TAG, "failed to confirm the new firmware; it will roll back on reboot");
    }
}
