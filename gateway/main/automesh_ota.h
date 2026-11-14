#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* Authenticated gateway-app-only OTA. NVS/Mesh credentials are untouched. */
esp_err_t automesh_ota_upload(httpd_req_t *req);

