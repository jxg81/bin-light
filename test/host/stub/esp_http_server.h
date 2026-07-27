#pragma once
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "esp_err.h"

#define HTTPD_400_BAD_REQUEST            400
#define HTTPD_403_FORBIDDEN              403
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

// Plantable request headers, and a recorder for the last error response, so
// the harness can drive the same-origin check through real handlers. NULL
// means "header absent" (ESP_ERR_NOT_FOUND), matching a non-browser client.
extern const char *stub_hdr_origin;
extern const char *stub_hdr_host;
extern int stub_last_err_code;

static inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    (void)r;
    const char *v = NULL;
    if (strcmp(field, "Origin") == 0) v = stub_hdr_origin;
    else if (strcmp(field, "Host") == 0) v = stub_hdr_host;
    if (v == NULL) return ESP_ERR_NOT_FOUND;
    if (strlen(v) >= val_size) return ESP_FAIL; // real API: ESP_ERR_HTTPD_RESULT_TRUNC
    strcpy(val, v);
    return ESP_OK;
}

static inline esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_OK; }
static inline esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
static inline esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) { (void)r; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t c, const char *m) { (void)r; (void)m; stub_last_err_code = c; return ESP_OK; }
static inline esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, int len) { (void)r; stub_capture(buf, len); return ESP_OK; }
// The harness can plant a POST body here to drive POST handlers.
extern const char *stub_post_body;
static inline int httpd_req_recv(httpd_req_t *r, char *b, size_t l)
{
    (void)r;
    if (!stub_post_body) return -1;
    size_t n = strlen(stub_post_body);
    if (n > l) n = l;
    memcpy(b, stub_post_body, n);
    return (int)n;
}
// Real implementation, matching ESP-IDF semantics: finds key=value in a
// urlencoded query/body string. The render harness and POST-parsing paths
// both depend on this actually working.
static inline esp_err_t httpd_query_key_value(const char *q, const char *k, char *v, size_t l)
{
    size_t klen = strlen(k);
    for (const char *p = q; p && *p; ) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen > klen && p[klen] == '=' && strncmp(p, k, klen) == 0) {
            size_t vlen = seglen - klen - 1;
            if (vlen >= l) vlen = l - 1;
            memcpy(v, p + klen + 1, vlen);
            v[vlen] = '\0';
            return ESP_OK;
        }
        if (seglen == klen && strncmp(p, k, klen) == 0) {  // bare key, no '='
            if (l > 0) v[0] = '\0';
            return ESP_OK;
        }
        p = amp ? amp + 1 : NULL;
    }
    return ESP_FAIL;
}
// The harness can plant a query string here to render a specific wizard step.
extern const char *stub_query_string;
static inline size_t httpd_req_get_url_query_len(httpd_req_t *r)
{ (void)r; return stub_query_string ? strlen(stub_query_string) : 0; }
static inline esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *b, size_t l)
{
    (void)r;
    if (!stub_query_string) return ESP_FAIL;
    snprintf(b, l, "%s", stub_query_string);
    return ESP_OK;
}
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u) { (void)h; (void)u; return ESP_OK; }
static inline esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *c) { (void)h; (void)c; return ESP_OK; }
