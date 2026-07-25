#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "led_state.h"

static const char *TAG = "wifi_manager";

#define WIFI_NVS_NAMESPACE "binlight"
#define WIFI_NVS_KEY       "wifi_cred_v1"
#define WIFI_CRED_VERSION  1

// How long a single provisioning connect attempt is given before the setup
// page reports failure. Generous: WPA handshake + DHCP on a busy 2.4GHz
// network can take a while.
#define PROVISION_ATTEMPT_TIMEOUT_MS (20 * 1000)
// In AutoAP-with-stored-credentials (router down at boot?), retry the stored
// network this often in the background. The AP stays up meanwhile, so a user
// can still re-provision a device whose old network is genuinely gone.
#define AUTOAP_RETRY_INTERVAL_MS     (60 * 1000)
// Keep serving the success page briefly before tearing the AP down under the
// user's phone.
#define AUTOAP_LINGER_MS             (4 * 1000)

#define SCAN_MAX_RECORDS 20
#define SCAN_MAX_SHOWN   15
#define PROV_HTML_BUF    6144
#define PROV_BODY_MAX    256

// Persisted Wi-Fi credentials (SPEC.md 3.4). Kconfig's compiled-in pair is
// retained only as a fallback for developer convenience - a stored blob
// always wins, and fresh devices with blank Kconfig go straight to AutoAP.
typedef struct {
    uint8_t version;
    char    ssid[33];      // 32 + NUL, per 802.11
    char    password[65];  // 64 + NUL
} wifi_cred_t;

typedef enum {
    WIFI_STATE_BOOT_CONNECTING, // first attempt with stored/Kconfig creds; bounded retries
    WIFI_STATE_AUTOAP,          // SoftAP up, provisioning page live; no automatic STA retries
    WIFI_STATE_RUNNING,         // connected at least once; reconnect forever on drops
} wifi_state_t;

#define BIT_CONNECTED    BIT0  // STA got an IP
#define BIT_BOOT_FAILED  BIT1  // boot-time retries exhausted
#define BIT_ATTEMPT_FAIL BIT2  // one AUTOAP-mode connect attempt failed
#define BIT_AUTOAP_EXIT  BIT3  // leave AutoAP: provisioned, or stored creds recovered

static EventGroupHandle_t s_wifi_event_group;
static volatile wifi_state_t s_state = WIFI_STATE_BOOT_CONNECTING;
static int s_retry_num = 0;
static bool s_connected = false;
static wifi_cred_t s_creds;          // the credentials currently being used
static bool s_have_creds = false;
static httpd_handle_t s_prov_server; // provisioning httpd, AutoAP mode only

// ---------------------------------------------------------------- creds ----

static bool load_creds(wifi_cred_t *out)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    size_t size = 0;
    bool ok = nvs_get_blob(handle, WIFI_NVS_KEY, NULL, &size) == ESP_OK &&
              size == sizeof(wifi_cred_t) &&
              nvs_get_blob(handle, WIFI_NVS_KEY, out, &size) == ESP_OK &&
              out->version == WIFI_CRED_VERSION &&
              out->ssid[0] != '\0';
    nvs_close(handle);
    return ok;
}

static void save_creds(const wifi_cred_t *creds)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "failed to open NVS to save credentials");
        return;
    }
    if (nvs_set_blob(handle, WIFI_NVS_KEY, creds, sizeof(*creds)) == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "saved credentials for \"%s\"", creds->ssid);
    }
    nvs_close(handle);
}

static void apply_sta_config(const wifi_cred_t *creds)
{
    // memcpy, not snprintf: these fields are fixed-width 802.11 byte arrays
    // that need no NUL when full (32/64 chars), and our buffers are one byte
    // larger to hold one - so a plain string copy looks like truncation to
    // the compiler and would actually drop the last character.
    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, creds->ssid, strnlen(creds->ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, creds->password, strnlen(creds->password, sizeof(wifi_config.sta.password)));
    // Threshold is the *minimum* security this station will accept from the
    // AP - WPA2 when we hold a password (WPA3-capable networks still pass),
    // open only for genuinely passwordless networks.
    wifi_config.sta.threshold.authmode = creds->password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

// ---------------------------------------------------------------- events ---

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // In AutoAP mode connects are driven explicitly (by the provisioning
        // handler or the stored-creds retry), never by the start event - with
        // no or wrong credentials an automatic connect would just thrash.
        if (s_state != WIFI_STATE_AUTOAP && s_have_creds) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        switch (s_state) {
        case WIFI_STATE_RUNNING:
            // Once it has been on the network, never permanently give up.
            ESP_LOGW(TAG, "wifi disconnected, retrying");
            esp_wifi_connect();
            break;
        case WIFI_STATE_BOOT_CONNECTING:
            if (s_retry_num < CONFIG_BINLIGHT_WIFI_MAXIMUM_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", s_retry_num, CONFIG_BINLIGHT_WIFI_MAXIMUM_RETRY);
            } else {
                ESP_LOGE(TAG, "failed to connect after %d attempts", s_retry_num);
                xEventGroupSetBits(s_wifi_event_group, BIT_BOOT_FAILED);
            }
            break;
        case WIFI_STATE_AUTOAP:
            xEventGroupSetBits(s_wifi_event_group, BIT_ATTEMPT_FAIL);
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, BIT_CONNECTED);
        if (s_state == WIFI_STATE_AUTOAP) {
            xEventGroupSetBits(s_wifi_event_group, BIT_AUTOAP_EXIT);
        }
    }
}

// ------------------------------------------------- provisioning web page ---

static int prov_append(char *buf, size_t buf_size, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= buf_size) {
        return (int)buf_size;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + off, buf_size - off, fmt, args);
    va_end(args);
    if (n < 0) {
        return off;
    }
    int new_off = off + n;
    return new_off > (int)buf_size ? (int)buf_size : new_off;
}

static void prov_url_decode(char *s)
{
    char *out = s;
    while (*s) {
        if (*s == '+') {
            *out++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

// Minimal HTML-escaping for scanned SSIDs, which are attacker-controlled
// bytes (anyone can broadcast any SSID at the device while it's in setup
// mode) and get embedded in both text and attribute contexts below.
static void prov_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 7 < out_size; p++) {
        switch (*p) {
        case '<':  o += snprintf(out + o, out_size - o, "&lt;"); break;
        case '>':  o += snprintf(out + o, out_size - o, "&gt;"); break;
        case '&':  o += snprintf(out + o, out_size - o, "&amp;"); break;
        case '\'': o += snprintf(out + o, out_size - o, "&#39;"); break;
        case '"':  o += snprintf(out + o, out_size - o, "&quot;"); break;
        default:   out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static int prov_page_head(char *buf, size_t size, int off)
{
    return prov_append(buf, size, off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Bin Light Setup</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em;}"
        ".note{color:#888;}"
        "input[type=text],input[type=password],select{width:100%%;max-width:20em;}"
        "p.field label{display:block;margin-bottom:.2em;}"
        "</style></head><body><h1>Bin Light Setup</h1>");
}

static esp_err_t prov_root_get_handler(httpd_req_t *req)
{
    char *html = malloc(PROV_HTML_BUF);
    if (html == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Blocking scan (~2s). Fine here: this server exists only to serve this
    // page, and a fresh scan is exactly what the user wants on reload.
    wifi_scan_config_t scan_config = {0};
    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * SCAN_MAX_RECORDS);
    uint16_t n_records = 0;
    if (records != NULL && esp_wifi_scan_start(&scan_config, true) == ESP_OK) {
        n_records = SCAN_MAX_RECORDS;
        if (esp_wifi_scan_get_ap_records(&n_records, records) != ESP_OK) {
            n_records = 0;
        }
    }

    int off = prov_page_head(html, PROV_HTML_BUF, 0);
    off = prov_append(html, PROV_HTML_BUF, off,
        "<p>Connect this bin light to your home Wi-Fi. Pick your network, enter "
        "its password, and press Connect.</p>"
        "<form method='POST' action='/provision'>"
        "<p class='field'><label for='ssid'>Network</label><select name='ssid' id='ssid'>");

    // Dedupe by SSID, strongest first, skipping hidden (empty) SSIDs.
    int shown = 0;
    for (int i = 0; i < n_records && shown < SCAN_MAX_SHOWN; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((const char *)records[j].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        char esc[33 * 6];
        prov_escape(ssid, esc, sizeof(esc));
        off = prov_append(html, PROV_HTML_BUF, off, "<option value='%s'>%s</option>", esc, esc);
        shown++;
    }
    free(records);

    off = prov_append(html, PROV_HTML_BUF, off,
        "<option value=''>Other (type below)&hellip;</option></select></p>"
        "<p class='field'><label for='ssid_custom'>Network name, if not listed (or hidden)</label>"
        "<input type='text' name='ssid_custom' id='ssid_custom' maxlength='32'></p>"
        "<p class='field'><label for='password'>Password</label>"
        "<input type='password' name='password' id='password' maxlength='64'></p>"
        "<p class='note'>Leave the password empty only if your network has none.</p>"
        "<p><button type='submit'>Connect</button></p>"
        "</form>"
        "<p class='note'>Networks found: %d. Reload this page to scan again.</p>"
        "</body></html>", shown);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(html);
    return ESP_OK;
}

static esp_err_t prov_provision_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= PROV_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
        return ESP_FAIL;
    }
    char body[PROV_BODY_MAX];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    wifi_cred_t creds = { .version = WIFI_CRED_VERSION };
    char ssid_custom[33] = "";
    httpd_query_key_value(body, "ssid", creds.ssid, sizeof(creds.ssid));
    httpd_query_key_value(body, "ssid_custom", ssid_custom, sizeof(ssid_custom));
    httpd_query_key_value(body, "password", creds.password, sizeof(creds.password));
    prov_url_decode(creds.ssid);
    prov_url_decode(ssid_custom);
    prov_url_decode(creds.password);
    if (ssid_custom[0] != '\0') {
        snprintf(creds.ssid, sizeof(creds.ssid), "%s", ssid_custom);
    }

    char *html = malloc(PROV_HTML_BUF);
    if (html == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int off = prov_page_head(html, PROV_HTML_BUF, 0);

    size_t pw_len = strlen(creds.password);
    if (creds.ssid[0] == '\0' || (pw_len > 0 && pw_len < 8)) {
        off = prov_append(html, PROV_HTML_BUF, off,
            "<p><b>%s</b></p><p><a href='/'>&larr; Back</a></p></body></html>",
            creds.ssid[0] == '\0' ? "Pick a network, or type its name in the second field."
                                   : "Wi-Fi passwords are at least 8 characters.");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, html, off);
        free(html);
        return ESP_OK;
    }

    // Try the credentials right now, synchronously: the response to this POST
    // *is* the outcome, keeping the whole flow JS-free. The AP interface
    // stays up (APSTA) so the phone keeps this connection while the STA side
    // attempts to join the real network.
    ESP_LOGI(TAG, "provisioning attempt for \"%s\"", creds.ssid);
    xEventGroupClearBits(s_wifi_event_group, BIT_CONNECTED | BIT_ATTEMPT_FAIL);
    apply_sta_config(&creds);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, BIT_CONNECTED | BIT_ATTEMPT_FAIL,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(PROVISION_ATTEMPT_TIMEOUT_MS));

    char esc_ssid[33 * 6];
    prov_escape(creds.ssid, esc_ssid, sizeof(esc_ssid));

    if (bits & BIT_CONNECTED) {
        // Persist only credentials that actually worked - a typo never
        // reaches NVS.
        save_creds(&creds);
        s_creds = creds;
        s_have_creds = true;
        off = prov_append(html, PROV_HTML_BUF, off,
            "<p><b>Connected to %s.</b></p>"
            "<p>This setup network will now disappear and the light will carry on "
            "with its normal job. Reconnect your phone to your own Wi-Fi, then find "
            "the light at <b>http://binlight.local</b> to set up its schedule.</p>"
            "</body></html>", esc_ssid);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, html, off);
        free(html);
        xEventGroupSetBits(s_wifi_event_group, BIT_AUTOAP_EXIT);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "provisioning attempt for \"%s\" failed", creds.ssid);
    off = prov_append(html, PROV_HTML_BUF, off,
        "<p><b>Couldn't join %s.</b></p>"
        "<p>Double-check the password (this is almost always the password) and that "
        "the network is a 2.4GHz one - this device can't see 5GHz-only networks.</p>"
        "<p><a href='/'>&larr; Try again</a></p></body></html>", esc_ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, off);
    free(html);
    return ESP_OK;
}

static esp_err_t prov_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    // The provisioning POST blocks its worker for up to
    // PROVISION_ATTEMPT_TIMEOUT_MS while the join attempt runs; give the
    // socket layer the same patience so the response still gets out.
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    esp_err_t err = httpd_start(&s_prov_server, &config);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = prov_root_get_handler };
    const httpd_uri_t provision = { .uri = "/provision", .method = HTTP_POST, .handler = prov_provision_post_handler };
    httpd_register_uri_handler(s_prov_server, &root);
    httpd_register_uri_handler(s_prov_server, &provision);
    return ESP_OK;
}

// ---------------------------------------------------------------- AutoAP ---

// Blocks until the device is provisioned and connected (or the stored
// network comes back). wifi must already be initialised and started.
static void run_autoap(void)
{
    s_state = WIFI_STATE_AUTOAP;

    static bool s_ap_netif_created = false;
    if (!s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }

    // SSID: "binlight-" + last 4 hex digits of the station MAC (SPEC.md 3.4)
    // - tells multiple devices apart on a scan with no user-entered name.
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    wifi_config_t ap_config = {0};
    int ssid_len = snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid),
                             "binlight-%02X%02X", mac[4], mac[5]);
    ap_config.ap.ssid_len = (uint8_t)ssid_len;
    // Open network, deliberately: a setup-mode password would have to be
    // printed on the device to be usable, and the AP exists only for the
    // minutes it takes to provision. Standard consumer-IoT trade-off.
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;
    ap_config.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    led_state_breathe_start((led_color_t){255, 255, 255});

    if (prov_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "failed to start provisioning server");
        // The AP is still up but useless; keep breathing so the failure is
        // at least visible, and keep retrying stored creds below.
    }

    ESP_LOGI(TAG, "AutoAP up: join \"%s\" and browse to http://192.168.4.1/", (char *)ap_config.ap.ssid);

    xEventGroupClearBits(s_wifi_event_group, BIT_AUTOAP_EXIT);
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, BIT_AUTOAP_EXIT,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(AUTOAP_RETRY_INTERVAL_MS));
        if (bits & BIT_AUTOAP_EXIT) {
            break;
        }
        if (s_have_creds) {
            // The stored network might just have been down (power outage
            // where the light boots faster than the router). Nudge a
            // background attempt; success lands as BIT_AUTOAP_EXIT.
            ESP_LOGI(TAG, "AutoAP: retrying stored network \"%s\"", s_creds.ssid);
            xEventGroupClearBits(s_wifi_event_group, BIT_ATTEMPT_FAIL);
            apply_sta_config(&s_creds); // a failed page attempt may have replaced it
            esp_wifi_connect();
        }
    }

    // Give the success page time to reach the phone before the AP vanishes.
    vTaskDelay(pdMS_TO_TICKS(AUTOAP_LINGER_MS));
    if (s_prov_server != NULL) {
        httpd_stop(s_prov_server);
        s_prov_server = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA); // drops the AP; the STA connection stays
    led_state_breathe_stop();
    s_state = WIFI_STATE_RUNNING;
    ESP_LOGI(TAG, "provisioned, AutoAP closed");
}

// ----------------------------------------------------------------- start ---

esp_err_t wifi_manager_start(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    // Credential priority: NVS (runtime-provisioned) wins; the compiled-in
    // Kconfig pair is a developer fallback; neither -> straight to AutoAP.
    if (load_creds(&s_creds)) {
        s_have_creds = true;
        ESP_LOGI(TAG, "using stored credentials for \"%s\"", s_creds.ssid);
    } else if (CONFIG_BINLIGHT_WIFI_SSID[0] != '\0') {
        s_creds.version = WIFI_CRED_VERSION;
        snprintf(s_creds.ssid, sizeof(s_creds.ssid), "%s", CONFIG_BINLIGHT_WIFI_SSID);
        snprintf(s_creds.password, sizeof(s_creds.password), "%s", CONFIG_BINLIGHT_WIFI_PASSWORD);
        s_have_creds = true;
        ESP_LOGI(TAG, "no stored credentials, using compiled-in fallback for \"%s\"", s_creds.ssid);
    }

    if (!s_have_creds) {
        ESP_LOGI(TAG, "no Wi-Fi credentials at all - starting AutoAP setup");
        s_state = WIFI_STATE_AUTOAP;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
        run_autoap();
        return ESP_OK;
    }

    s_state = WIFI_STATE_BOOT_CONNECTING;
    apply_sta_config(&s_creds);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to SSID:%s", s_creds.ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            BIT_CONNECTED | BIT_BOOT_FAILED,
                                            pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & BIT_CONNECTED) {
        s_state = WIFI_STATE_RUNNING;
        return ESP_OK;
    }

    // Couldn't reach the stored network. Open AutoAP so the device can be
    // re-provisioned (moved house, new router), while retrying the stored
    // network in the background (router merely rebooting).
    ESP_LOGW(TAG, "could not join \"%s\" - opening AutoAP while retrying in the background", s_creds.ssid);
    run_autoap();
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_current_ssid(void)
{
    return s_have_creds ? s_creds.ssid : "";
}

esp_err_t wifi_manager_forget_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, WIFI_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; // nothing stored is the state we wanted anyway
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "stored Wi-Fi credentials erased - next boot will enter AutoAP setup");
    }
    return err;
}
