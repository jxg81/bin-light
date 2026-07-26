#include "captive_dns.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

static const char *TAG = "captive_dns";

#define DNS_PORT            53
#define DNS_BUF_SIZE        512   // the classic UDP DNS limit; anything larger is not a probe
#define DNS_HEADER_LEN      12
#define DNS_RECV_TIMEOUT_MS 500   // how often the task checks whether it should exit
#define DNS_TASK_STACK      3072
#define DNS_TASK_PRIO       5

// Header field offsets, in bytes from the start of the message.
#define OFF_ID       0
#define OFF_FLAGS    2
#define OFF_QDCOUNT  4
#define OFF_ANCOUNT  6

#define FLAG_QR      0x8000  // this is a response
#define FLAG_AA      0x0400  // authoritative
#define FLAG_RA      0x0080  // recursion available
#define FLAG_RCODE   0x000F
#define FLAG_OPCODE  0x7800

#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1

static TaskHandle_t s_task;
static volatile bool s_running;
static int s_sock = -1;
static uint32_t s_answer_addr;  // network byte order, ready to memcpy into rdata

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

// Walks the QNAME label sequence and returns the offset just past the
// terminating zero byte, or -1 if it runs off the end or hits a compression
// pointer. A query has nothing to compress against, so a pointer here means a
// malformed or hostile packet and the right answer is to drop it.
static int skip_qname(const uint8_t *buf, int len, int off)
{
    while (off < len) {
        uint8_t label = buf[off];
        if (label == 0) {
            return off + 1;
        }
        if ((label & 0xC0) != 0) {
            return -1; // compression pointer
        }
        off += 1 + label;
    }
    return -1;
}

// Builds the reply in place: the question is echoed back verbatim (clients
// match on it) with a single A record appended. Returns the total reply
// length, or -1 to send nothing.
static int build_reply(uint8_t *buf, int len)
{
    if (len < DNS_HEADER_LEN) {
        return -1;
    }

    uint16_t flags = rd16(buf + OFF_FLAGS);
    if (flags & FLAG_QR) {
        return -1; // already a response, not a question for us
    }
    if (flags & FLAG_OPCODE) {
        return -1; // not a standard QUERY (inverse query, status, notify...)
    }
    if (rd16(buf + OFF_QDCOUNT) < 1) {
        return -1;
    }

    int qname_end = skip_qname(buf, len, DNS_HEADER_LEN);
    if (qname_end < 0 || qname_end + 4 > len) {
        return -1;
    }
    uint16_t qtype = rd16(buf + qname_end);
    uint16_t qclass = rd16(buf + qname_end + 2);
    int question_end = qname_end + 4;

    // Answer the question section as a response either way. For anything that
    // is not an internet A record - AAAA above all, which every phone asks
    // for alongside A - reply with zero answers rather than staying silent:
    // an empty NOERROR makes the client fall straight through to the A
    // result, where a timeout would stall it for seconds.
    bool answer = (qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN);

    flags = (uint16_t)((flags & ~FLAG_RCODE) | FLAG_QR | FLAG_AA | FLAG_RA);
    wr16(buf + OFF_FLAGS, flags);
    wr16(buf + OFF_QDCOUNT, 1);
    wr16(buf + OFF_ANCOUNT, answer ? 1 : 0);
    // Drop any authority/additional records the query carried (EDNS OPT, most
    // often) - they are echoed as counts we are not going to reproduce.
    wr16(buf + 8, 0);
    wr16(buf + 10, 0);

    if (!answer) {
        return question_end;
    }

    if (question_end + 16 > DNS_BUF_SIZE) {
        return -1;
    }
    uint8_t *a = buf + question_end;
    a[0] = 0xC0;                    // name: pointer...
    a[1] = DNS_HEADER_LEN;          // ...to the question's QNAME
    wr16(a + 2, DNS_TYPE_A);
    wr16(a + 4, DNS_CLASS_IN);
    a[6] = a[7] = a[8] = a[9] = 0;  // TTL 0 - see the header's note on caching
    wr16(a + 10, 4);                // rdlength
    memcpy(a + 12, &s_answer_addr, 4);

    return question_end + 16;
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_BUF_SIZE];

    while (s_running) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue; // timeout (the normal case) or the socket going away on stop
        }
        int reply_len = build_reply(buf, len);
        if (reply_len > 0) {
            sendto(s_sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    // Hand out whatever the SoftAP is actually on rather than assuming the
    // default, so changing the AP subnet can't silently break the portal.
    s_answer_addr = 0;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info;
    if (ap != NULL && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        s_answer_addr = ip_info.ip.addr; // already network byte order
    } else {
        s_answer_addr = ipaddr_addr("192.168.4.1");
        ESP_LOGW(TAG, "could not read the SoftAP address, defaulting to 192.168.4.1");
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    // Bounded receive, so the task wakes often enough to notice s_running
    // going false instead of blocking forever on a quiet network.
    struct timeval tv = { .tv_sec = 0, .tv_usec = DNS_RECV_TIMEOUT_MS * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(:53) failed: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    s_running = true;
    if (xTaskCreate(dns_task, "captive_dns", DNS_TASK_STACK, NULL, DNS_TASK_PRIO, &s_task) != pdPASS) {
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "captive DNS up: every name resolves to " IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    // Wait out one receive timeout so the task has left the socket alone
    // before it is closed, rather than closing underneath a blocked
    // recvfrom().
    for (int i = 0; i < 10 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(DNS_RECV_TIMEOUT_MS / 4));
    }

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    ESP_LOGI(TAG, "captive DNS stopped");
}
