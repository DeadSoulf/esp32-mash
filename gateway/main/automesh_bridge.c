#include "automesh_bridge.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "nvs.h"
#include "esp_timer.h"
#include "automesh_status_led.h"

#define MAX_CANDIDATES 24
static const char *TAG = "AUTOMESH";
static SemaphoreHandle_t lock;
typedef struct {
    esp_ble_mesh_unprov_dev_add_t device;
    bool occupied;
    bool rejected;
    bool approved;
    uint16_t mesh_address;
    int64_t last_reply_us;
} candidate_t;
static candidate_t candidates[MAX_CANDIDATES];

static void hex_bytes(const uint8_t *p, size_t size, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 0xf];
    }
    out[size * 2] = 0;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool from_hex(const char *in, uint8_t *out, size_t length)
{
    if (strlen(in) != length * 2) return false;
    for (size_t i = 0; i < length; ++i) {
        int hi = nibble(in[i * 2]), lo = nibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (hi << 4) | lo;
    }
    return true;
}

esp_err_t automesh_load_appkey(uint8_t key[16])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("automesh", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    size_t length = 16;
    err = nvs_get_blob(h, "appkey", key, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(key, 16);
        err = nvs_set_blob(h, "appkey", key, 16);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "AppKey NVS failed: %s", esp_err_to_name(err));
    return err;
}

void automesh_bridge_init(void)
{
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
}

void automesh_candidate(const uint8_t uuid[16], const uint8_t addr[6],
                        esp_ble_mesh_addr_type_t type, uint16_t oob_info,
                        esp_ble_mesh_prov_bearer_t bearer)
{
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    int empty = -1, index = -1;
    for (int i = 0; i < MAX_CANDIDATES; ++i) {
        if (!candidates[i].occupied && empty < 0) empty = i;
        if (candidates[i].occupied && memcmp(candidates[i].device.uuid, uuid, 16) == 0) {
            index = i;
            break;
        }
    }
    bool newly_found = false;
    if (index < 0 && empty >= 0) {
        index = empty;
        candidate_t *c = &candidates[index];
        memset(c, 0, sizeof(*c));
        c->occupied = true;
        memcpy(c->device.uuid, uuid, 16);
        newly_found = true;
    }
    if (index >= 0) {
        candidate_t *c = &candidates[index];
        memcpy(c->device.addr, addr, 6);
        c->device.addr_type = type;
        c->device.oob_info = oob_info;
        c->device.bearer = bearer;
    }
    xSemaphoreGive(lock);
    if (newly_found) {
        char u[33], a[13];
        hex_bytes(uuid, 16, u);
        hex_bytes(addr, 6, a);
        printf("AUTOMESH {\"event\":\"candidate\",\"uuid\":\"%s\",\"addr\":\"%s\",\"oob_info\":%u}\n", u, a, oob_info);
        fflush(stdout);
    }
}

void automesh_provisioned(const uint8_t uuid[16], uint16_t address)
{
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < MAX_CANDIDATES; i++) {
            if (candidates[i].occupied && !memcmp(candidates[i].device.uuid,uuid,16)) {
                candidates[i].approved = true;
                candidates[i].mesh_address = address;
                break;
            }
        }
        xSemaphoreGive(lock);
    }
    automesh_led_set_registered(true);
    char u[33];
    hex_bytes(uuid, 16, u);
    printf("AUTOMESH {\"event\":\"provisioned\",\"uuid\":\"%s\",\"address\":%u}\n", u, address);
    fflush(stdout);
}


void automesh_bridge_restore(const uint8_t uuid[16], uint16_t address)
{
    if (!uuid || !address || !lock)return;
    if (xSemaphoreTake(lock,pdMS_TO_TICKS(1000))!=pdTRUE)return;
    for(int i=0;i<MAX_CANDIDATES;i++) {
        candidate_t *c=&candidates[i];
        if (c->occupied && !memcmp(c->device.uuid,uuid,16)) {
            c->approved=true;
            c->rejected=false;
            c->mesh_address=address;
            xSemaphoreGive(lock);
            automesh_led_set_registered(true);
            return;
        }
    }
    for(int i=0;i<MAX_CANDIDATES;i++) {
        candidate_t *c=&candidates[i];
        if (!c->occupied) {
            memset(c,0,sizeof(*c));
            c->occupied=true;
            c->approved=true;
            c->mesh_address=address;
            memcpy(c->device.uuid,uuid,16);
            break;
        }
    }
    xSemaphoreGive(lock);
    automesh_led_set_registered(true);
}

void automesh_provisioning_closed(void)
{
    if (!lock || xSemaphoreTake(lock,pdMS_TO_TICKS(100))!=pdTRUE)return;
    for(int i=0;i<MAX_CANDIDATES;i++) {
        if (candidates[i].occupied && candidates[i].mesh_address==0 &&
            candidates[i].approved) candidates[i].approved=false;
    }
    xSemaphoreGive(lock);
}

void automesh_note_reply(uint16_t mesh_address)
{
    bool found = false;
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    for (int i=0; i<MAX_CANDIDATES; ++i) {
        if (candidates[i].occupied && candidates[i].mesh_address == mesh_address) {
            candidates[i].last_reply_us = esp_timer_get_time();
            found = true;
            break;
        }
    }
    xSemaphoreGive(lock);
    if (found) automesh_led_mark_alive();
}

esp_err_t automesh_deny_uuid(const char *uuid_hex)
{
    uint8_t uuid[16];
    if (!uuid_hex || !from_hex(uuid_hex,uuid,16)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(lock,pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret=ESP_ERR_NOT_FOUND;
    for (int i=0; i<MAX_CANDIDATES; ++i) {
        if (candidates[i].occupied && !memcmp(candidates[i].device.uuid,uuid,16)) {
            if (candidates[i].mesh_address) ret=ESP_ERR_INVALID_STATE;
            else {candidates[i].rejected=true; ret=ESP_OK;}
            break;
        }
    }
    xSemaphoreGive(lock);
    return ret;
}

esp_err_t automesh_approve_uuid(const char *uuid_hex, const char *oob_hex)
{
    uint8_t uuid[16],oob[16];
    if (!uuid_hex || !oob_hex || !from_hex(uuid_hex,uuid,16) ||
        !from_hex(oob_hex,oob,16)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(lock,pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_ble_mesh_unprov_dev_add_t device={0};
    int index=-1;
    for (int i=0; i<MAX_CANDIDATES; ++i) {
        if (candidates[i].occupied && candidates[i].approved &&
            candidates[i].mesh_address == 0) {
            xSemaphoreGive(lock);
            memset(oob,0,sizeof(oob));
            return ESP_ERR_INVALID_STATE; // one pending provisioning at a time
        }
        if (candidates[i].occupied && !memcmp(candidates[i].device.uuid,uuid,16) &&
            !candidates[i].rejected && !candidates[i].approved) {
            index=i;
            device=candidates[i].device;
        }
    }
    if(index < 0) {
        xSemaphoreGive(lock);
        memset(oob,0,sizeof(oob));
        return ESP_ERR_NOT_FOUND;
    }
    candidates[index].approved=true;
    xSemaphoreGive(lock);

    esp_err_t err=esp_ble_mesh_provisioner_set_static_oob_value(oob,16);
    memset(oob,0,sizeof(oob));
    if(err == ESP_OK) err=esp_ble_mesh_provisioner_add_unprov_dev(&device,
       (esp_ble_mesh_dev_add_flag_t)(ADD_DEV_RM_AFTER_PROV_FLAG |
        ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG));
    if(err != ESP_OK && xSemaphoreTake(lock,pdMS_TO_TICKS(1000))==pdTRUE) {
        candidates[index].approved=false;
        xSemaphoreGive(lock);
    }
    return err;
}

int automesh_devices_json(char *out,size_t cap)
{
    if(!out || cap < 40) return -1;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(500))!=pdTRUE) return -1;
    size_t used=0;
    int n=snprintf(out,cap,"{\"devices\":[");
    if(n<0 || (size_t)n>=cap){xSemaphoreGive(lock);return -1;}
    used=n;
    bool first=true;
    int64_t now=esp_timer_get_time();
    for (int i=0;i<MAX_CANDIDATES;i++) {
        candidate_t *c=&candidates[i];
        if(!c->occupied)continue;
        char uuid[33],mac[13];
        hex_bytes(c->device.uuid,16,uuid);
        hex_bytes(c->device.addr,6,mac);
        bool alive=c->mesh_address && c->last_reply_us &&
                   (now-c->last_reply_us < 35000000LL);
        const char *status=c->rejected?"denied":
             c->mesh_address?(alive?"online":"offline"):
             c->approved?"provisioning":"pending";
        n=snprintf(out+used,cap-used,
          "%s{\"uuid\":\"%s\",\"mac\":\"%s\",\"state\":\"%s\",\"address\":%u,\"online\":%s}",
          first?"":",",uuid,mac,status,(unsigned)c->mesh_address,alive?"true":"false");
        if(n<0 || (size_t)n >= cap-used){xSemaphoreGive(lock);return -1;}
        used+=(size_t)n;
        first=false;
    }
    xSemaphoreGive(lock);
    n=snprintf(out+used,cap-used,"]}");
    if(n<0 || (size_t)n >= cap-used)return -1;
    used+=(size_t)n;
    return (int)used;
}

static void list_candidates(void)
{
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) return;
    for (int i = 0; i < MAX_CANDIDATES; ++i) {
        candidate_t *c = &candidates[i];
        if (!c->occupied || c->rejected) continue;
        char u[33], a[13];
        hex_bytes(c->device.uuid, 16, u);
        hex_bytes(c->device.addr, 6, a);
        printf("AUTOMESH {\"event\":\"candidate\",\"uuid\":\"%s\",\"addr\":\"%s\",\"approved\":%s}\n",
               u, a, c->approved ? "true" : "false");
    }
    xSemaphoreGive(lock);
    fflush(stdout);
}

static void command_task(void *arg)
{
    char line[160];
    printf("AUTOMESH {\"event\":\"ready\",\"version\":\"0.1.0\"}\n");
    fflush(stdout);
    while (true) {
        if (!fgets(line, sizeof(line), stdin)) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        char verb[16] = {0}, uuid_text[40] = {0}, oob_text[40] = {0};
        int count = sscanf(line, "%15s %39s %39s", verb, uuid_text, oob_text);
        if (count == 1 && strcmp(verb, "LIST") == 0) {
            list_candidates();
            continue;
        }
        if (count == 1 && strcmp(verb, "HELLO") == 0) {
            printf("AUTOMESH {\"event\":\"ready\",\"version\":\"0.1.0\"}\n");
            fflush(stdout);
            continue;
        }
        uint8_t uuid[16], oob[16];
        if (count < 2 || !from_hex(uuid_text, uuid, 16)) {
            ESP_LOGW(TAG, "Invalid command");
            continue;
        }
        if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) continue;
        int idx = -1;
        for (int i = 0; i < MAX_CANDIDATES; ++i)
            if (candidates[i].occupied && !memcmp(candidates[i].device.uuid, uuid, 16)) { idx = i; break; }
        esp_ble_mesh_unprov_dev_add_t device = {0};
        bool approved = false;
        if (idx >= 0) {
            if (strcmp(verb, "DENY") == 0) candidates[idx].rejected = true;
            if (strcmp(verb, "APPROVE") == 0 && !candidates[idx].approved && !candidates[idx].rejected) {
                device = candidates[idx].device;
                approved = true;
            }
        }
        xSemaphoreGive(lock);
        char u[33];
        hex_bytes(uuid, 16, u);
        if (idx < 0) {
            printf("AUTOMESH {\"event\":\"error\",\"uuid\":\"%s\",\"reason\":\"not_found\"}\n", u);
        } else if (strcmp(verb, "DENY") == 0) {
            printf("AUTOMESH {\"event\":\"denied\",\"uuid\":\"%s\"}\n", u);
        } else if (strcmp(verb, "APPROVE") == 0 && approved && count == 3 &&
                   from_hex(oob_text, oob, sizeof(oob))) {
            // An OOB value must also be preconfigured on the physical node.
            esp_err_t err = esp_ble_mesh_provisioner_set_static_oob_value(oob, sizeof(oob));
            memset(oob, 0, sizeof(oob));
            if (err == ESP_OK) {
                err = esp_ble_mesh_provisioner_add_unprov_dev(&device,
                    (esp_ble_mesh_dev_add_flag_t)(ADD_DEV_RM_AFTER_PROV_FLAG |
                    ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG));
            }
            if (err == ESP_OK) {
                if (xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE) {
                    if (idx >= 0) candidates[idx].approved = true;
                    xSemaphoreGive(lock);
                }
                printf("AUTOMESH {\"event\":\"approval_started\",\"uuid\":\"%s\"}\n", u);
            } else {
                printf("AUTOMESH {\"event\":\"error\",\"uuid\":\"%s\",\"reason\":\"mesh_error\"}\n", u);
            }
        } else {
            printf("AUTOMESH {\"event\":\"error\",\"uuid\":\"%s\",\"reason\":\"approval_needs_16_byte_oob\"}\n", u);
        }
        fflush(stdout);
    }
}

void automesh_bridge_start(void)
{
    BaseType_t ok = xTaskCreate(command_task, "automesh_usb", 4096, NULL, 5, NULL);
    if (ok != pdPASS) ESP_LOGE(TAG, "USB command task creation failed");
}

