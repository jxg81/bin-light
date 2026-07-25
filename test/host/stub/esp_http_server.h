#pragma once
#include <stddef.h>
#include "esp_err.h"

#define HTTPD_400_BAD_REQUEST            400
#define HTTPD_500_INTERNAL_SERVER_ERROR  500
#define HTTPD_SOCK_ERR_TIMEOUT           (-3)
#define HTTPD_RESP_USE_STRLEN            (-1)

typedef int httpd_err_code_t;
typedef void *httpd_handle_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 2 } httpd_method_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int            method;
    const char    *uri;
    size_t         content_len;
    void          *user_ctx;
} httpd_req_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    unsigned max_uri_handlers;
    unsigned max_open_sockets;
    unsigned lru_purge_enable;
    unsigned stack_size;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() (httpd_config_t){ .max_uri_handlers = 8, .lru_purge_enable = 1, .stack_size = 4096 }

// Capture hooks provided by the harness.
void stub_capture(const char *buf, int len);

static inline esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_OK; }
static inline esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
static inline esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) { (void)r; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t c, const char *m) { (void)r; (void)c; (void)m; return ESP_OK; }
static inline esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, int len) { (void)r; stub_capture(buf, len); return ESP_OK; }
static inline int httpd_req_recv(httpd_req_t *r, char *b, size_t l) { (void)r; (void)b; (void)l; return -1; }
static inline esp_err_t httpd_query_key_value(const char *q, const char *k, char *v, size_t l) { (void)q; (void)k; (void)v; (void)l; return ESP_FAIL; }
static inline size_t httpd_req_get_url_query_len(httpd_req_t *r) { (void)r; return 0; }
static inline esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *b, size_t l) { (void)r; (void)b; (void)l; return ESP_FAIL; }
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u) { (void)h; (void)u; return ESP_OK; }
static inline esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *c) { (void)h; (void)c; return ESP_OK; }
