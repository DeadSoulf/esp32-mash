set(srcs "main.c"
    "board.c")

idf_component_register(SRCS "automesh_status_led.c" "automesh_node_ota.c" "${srcs}"
                    INCLUDE_DIRS  "."
                    REQUIRES nvs_flash app_update esp_hw_support bt esp_timer esp_driver_gpio esp_driver_rmt esp_app_format mbedtls)

