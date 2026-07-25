idf_component_register(SRCS "main.c" "led_state.c" "wifi_manager.c" "time_sync.c" "schedule.c" "web_server.c" "settings.c" "waste_api.c" "councils.c" "date_parse.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client mbedtls)
