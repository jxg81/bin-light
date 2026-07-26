#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;

#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), \
                  (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)

// No AP netif on the host, so captive_dns_start() takes its documented
// fallback path. The tests never call it.
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k) { (void)k; return 0; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i)
{ (void)n; (void)i; return -1; }
