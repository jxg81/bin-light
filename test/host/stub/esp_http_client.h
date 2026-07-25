#pragma once
#include <stddef.h>
#include "esp_err.h"

// Host stub: the test harness implements these four functions and serves
// captured fixture files by URL (see test_backends.c), driving the real
// event-handler/append path in waste_api.c with real bytes.

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_DATA,
} esp_http_client_event_id_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    void  *user_data;
    void  *data;
    int    data_len;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef struct {
    const char           *url;
    http_event_handle_cb  event_handler;
    void                 *user_data;
    esp_err_t           (*crt_bundle_attach)(void *conf);
    int                   timeout_ms;
} esp_http_client_config_t;

typedef struct stub_http_client *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
