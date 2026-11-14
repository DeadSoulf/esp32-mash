#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Background scan available only during gateway Wi-Fi commissioning. */
esp_err_t automesh_wifi_scan_init(void);
esp_err_t automesh_wifi_scan_begin(void);
char *automesh_wifi_scan_json(void); /* Caller must free(). */

