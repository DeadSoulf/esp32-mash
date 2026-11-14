#include "automesh_admin.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "nvs.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"

#define ADMIN_ITERATIONS 12000u
#define ADMIN_MARKER 0x414d4144u
#define ADMIN_SALT_SIZE 16u
#define ADMIN_HASH_SIZE 32u
#define LEGACY_ADMIN_LEN 32u

typedef struct {
    uint32_t marker;
    uint32_t rounds;
    uint8_t salt[ADMIN_SALT_SIZE];
    uint8_t digest[ADMIN_HASH_SIZE];
} admin_record_t;

typedef enum {
    ADMIN_UNSET = 0,
    ADMIN_LEGACY = 1,
    ADMIN_HASHED = 2,
} admin_kind_t;

static admin_record_t saved;
static char legacy_key[LEGACY_ADMIN_LEN + 1];
static admin_kind_t admin_kind = ADMIN_UNSET;

static int count_valid_utf8(const char *str)
{
    if (!str) return -1;
    size_t bytes = strlen(str);
    if (bytes > AUTOMESH_ADMIN_MAX_BYTES) return -1;
    int count = 0;
    const uint8_t *p = (const uint8_t *)str;
    for (size_t i = 0; i < bytes;) {
        uint32_t cp;
        uint8_t c = p[i++];
        if (c < 0x20 || c == 0x7f) return -1; /* no control chars */
        if (c < 0x80) {
            cp = c;
        } else {
            unsigned extra;
            if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
            else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
            else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
            else return -1;
            if (i + extra > bytes) return -1;
            for (unsigned j = 0; j < extra; j++) {
                if ((p[i] & 0xc0) != 0x80) return -1;
                cp = (cp << 6) | (p[i++] & 0x3f);
            }
            if ((extra == 1 && cp < 0x80) ||
                (extra == 2 && cp < 0x800) ||
                (extra == 3 && cp < 0x10000) ||
                (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
                return -1;
        }
        count++;
    }
    return count;
}

static bool password_valid(const char *password)
{
    const int count = count_valid_utf8(password);
    return count >= 6 && count <= 64;
}

static bool constant_time_bytes(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t mismatch = 0;
    for (size_t i = 0; i < len; i++) mismatch |= a[i] ^ b[i];
    return mismatch == 0;
}

static int derive(const admin_record_t *record, const char *password,
                  uint8_t digest[ADMIN_HASH_SIZE])
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, (const unsigned char *)password, strlen(password),
        record->salt, ADMIN_SALT_SIZE, record->rounds, ADMIN_HASH_SIZE, digest);
}

esp_err_t automesh_admin_init(void)
{
    admin_kind = ADMIN_UNSET;
    memset(&saved, 0, sizeof(saved));
    memset(legacy_key, 0, sizeof(legacy_key));

    nvs_handle_t h;
    esp_err_t e = nvs_open("meshweb", NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    size_t size = sizeof(saved);
    e = nvs_get_blob(h, "admin_v1", &saved, &size);
    if (e == ESP_OK) {
        nvs_close(h);
        if (size != sizeof(saved) || saved.marker != ADMIN_MARKER ||
            saved.rounds != ADMIN_ITERATIONS) {
            memset(&saved, 0, sizeof(saved));
            return ESP_ERR_INVALID_STATE; /* never reopen admin setup on corrupt NVS */
        }
        admin_kind = ADMIN_HASHED;
        return ESP_OK;
    }
    if (e != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return e;
    }
    size = sizeof(legacy_key);
    e = nvs_get_str(h, "admin", legacy_key, &size);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    if (strlen(legacy_key) != LEGACY_ADMIN_LEN) return ESP_ERR_INVALID_STATE;
    admin_kind = ADMIN_LEGACY;
    return ESP_OK;
}

bool automesh_admin_is_configured(void)
{
    return admin_kind != ADMIN_UNSET;
}

bool automesh_admin_verify(const char *password)
{
    if (!password || !automesh_admin_is_configured()) return false;
    if (admin_kind == ADMIN_LEGACY) {
        if (strlen(password) != LEGACY_ADMIN_LEN) return false;
        return constant_time_bytes((const uint8_t *)password,
                                   (const uint8_t *)legacy_key, LEGACY_ADMIN_LEN);
    }
    if (strlen(password) > AUTOMESH_ADMIN_MAX_BYTES) return false;
    uint8_t derived[ADMIN_HASH_SIZE] = {0};
    const bool ok = derive(&saved, password, derived) == 0 &&
                    constant_time_bytes(derived, saved.digest, ADMIN_HASH_SIZE);
    mbedtls_platform_zeroize(derived, sizeof(derived));
    return ok;
}

static esp_err_t save_password(const char *password)
{
    if (!password_valid(password)) return ESP_ERR_INVALID_ARG;

    admin_record_t replacement = {
        .marker = ADMIN_MARKER,
        .rounds = ADMIN_ITERATIONS,
    };
    esp_fill_random(replacement.salt, sizeof(replacement.salt));
    if (derive(&replacement, password, replacement.digest) != 0) {
        mbedtls_platform_zeroize(&replacement, sizeof(replacement));
        return ESP_FAIL;
    }

    nvs_handle_t h;
    esp_err_t e = nvs_open("meshweb", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "admin_v1", &replacement, sizeof(replacement));
        if (e == ESP_OK) {
            esp_err_t old = nvs_erase_key(h, "admin");
            if (old != ESP_OK && old != ESP_ERR_NVS_NOT_FOUND) e = old;
        }
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        saved = replacement;
        admin_kind = ADMIN_HASHED;
        mbedtls_platform_zeroize(legacy_key, sizeof(legacy_key));
    }
    mbedtls_platform_zeroize(&replacement, sizeof(replacement));
    return e;
}

esp_err_t automesh_admin_set_initial(const char *password)
{
    if (automesh_admin_is_configured()) return ESP_ERR_INVALID_STATE;
    return save_password(password);
}

esp_err_t automesh_admin_change(const char *password)
{
    if (!automesh_admin_is_configured()) return ESP_ERR_INVALID_STATE;
    return save_password(password);
}

