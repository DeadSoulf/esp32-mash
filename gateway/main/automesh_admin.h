#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Administrator credential is stored exclusively in gateway NVS.
 * New credentials are PBKDF2-HMAC-SHA256 hashes with per-device random salt.
 * Old 32-hex factory-generated token is read-only for migration and can be
 * replaced by the administrator without erasing Mesh or Wi-Fi settings.
 */
#define AUTOMESH_ADMIN_MAX_BYTES 64
esp_err_t automesh_admin_init(void);
bool automesh_admin_is_configured(void);
bool automesh_admin_verify(const char *password);
esp_err_t automesh_admin_set_initial(const char *password);
esp_err_t automesh_admin_change(const char *password);

