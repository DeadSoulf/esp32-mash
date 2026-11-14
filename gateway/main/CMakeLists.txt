set(srcs "main.c" "automesh_bridge.c" "automesh_status_led.c" "automesh_web.c" "automesh_ota.c" "automesh_gateway_node_ota.c" "automesh_admin.c" "automesh_wifi_scan.c")

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS  "."
                    EMBED_TXTFILES "gateway_index.html"
                    REQUIRES esp_wifi esp_netif esp_event esp_http_server json nvs_flash app_update esp_timer esp_driver_gpio esp_hw_support bt mbedtls esp_driver_rmt)

# Embedded UI is served directly from gateway flash.



