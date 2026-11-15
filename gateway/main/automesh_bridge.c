#include "automesh_bridge.h"
#include "automesh_ota_protocol.h"
#include <stdio.h>
#include <stdlib.h>
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
#include "esp_ble_mesh_networking_api.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "automesh_status_led.h"
#include "automesh_gateway_node_ota.h"
#include "cJSON.h"

#define MAX_CANDIDATES 24
#define NODE_NAME_BYTES 48
#define REGISTRY_MAGIC 0x41554d32u /* AUM2 */
static const char *TAG = "AUTOMESH";
static SemaphoreHandle_t lock;
typedef struct {
    esp_ble_mesh_unprov_dev_add_t device;
    bool occupied;
    bool rejected;
    bool approved;
    bool removing;
    uint16_t mesh_address;
    int64_t last_reply_us;
    char name[NODE_NAME_BYTES+1];
    char fw_version[32];  // RAM only; received from the node after boot
} candidate_t;
static candidate_t candidates[MAX_CANDIDATES];
static bool provisioner_ready;
static bool removal_pending;
static uint8_t removal_uuid[16];
typedef struct {
    uint8_t uuid[16];
    uint16_t address;
    char name[NODE_NAME_BYTES+1];
} registered_node_t;
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    registered_node_t entries[MAX_CANDIDATES];
} registry_blob_t;

/* Only metadata is stored here; Mesh keys remain owned by the SDK's NVS. */
static esp_err_t registry_save_locked(void)
{
    registry_blob_t db = {.magic=REGISTRY_MAGIC,.version=1};
    for(int i=0;i<MAX_CANDIDATES;i++) {
        const candidate_t *c=&candidates[i];
        if(!c->occupied || !c->mesh_address) continue;
        registered_node_t *r=&db.entries[db.count++];
        memcpy(r->uuid,c->device.uuid,16);
        r->address=c->mesh_address;
        memcpy(r->name,c->name,sizeof(r->name));
    }
    nvs_handle_t h;
    esp_err_t err=nvs_open("meshlist",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    err=nvs_set_blob(h,"registered",&db,sizeof(db));
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    return err;
}
static void registry_load(void)
{
    registry_blob_t db={0};
    size_t len=sizeof(db);
    nvs_handle_t h;
    if(nvs_open("meshlist",NVS_READONLY,&h)!=ESP_OK)return;
    esp_err_t err=nvs_get_blob(h,"registered",&db,&len);
    nvs_close(h);
    if(err!=ESP_OK || len!=sizeof(db) || db.magic!=REGISTRY_MAGIC ||
       db.version!=1 || db.count>MAX_CANDIDATES) {
        ESP_LOGW("AUTOMESH","No valid saved device registry (%s)",esp_err_to_name(err));
        return;
    }
    for(int i=0;i<db.count;i++) {
        registered_node_t *r=&db.entries[i];
        if(!ESP_BLE_MESH_ADDR_IS_UNICAST(r->address) ||
           r->name[NODE_NAME_BYTES]!='\0')continue;
        candidate_t *c=&candidates[i];
        c->occupied=true;
        c->approved=true;
        c->mesh_address=r->address;
        memcpy(c->device.uuid,r->uuid,16);
        memcpy(c->name,r->name,sizeof(c->name));
    }
    ESP_LOGI("AUTOMESH","Recovered %u device registry records from NVS",db.count);
}


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
    registry_load();
}
void automesh_bridge_mesh_ready(void)
{
    provisioner_ready=true;
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
    automesh_bridge_restore(uuid,address);
    char u[33];
    hex_bytes(uuid,16,u);
    printf("AUTOMESH {\"event\":\"provisioned\",\"uuid\":\"%s\",\"address\":%u}\n",u,address);
    fflush(stdout);
}

void automesh_bridge_restore(const uint8_t uuid[16], uint16_t address)
{
    if(!uuid || !ESP_BLE_MESH_ADDR_IS_UNICAST(address) || !lock)return;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(1000))!=pdTRUE)return;
    int match=-1,free_index=-1;
    for(int i=0;i<MAX_CANDIDATES;i++) {
        candidate_t *c=&candidates[i];
        if(!c->occupied && free_index<0)free_index=i;
        if(c->occupied && !memcmp(c->device.uuid,uuid,16)) {match=i;break;}
    }
    if(match<0)match=free_index;
    if(match>=0) {
        candidate_t *c=&candidates[match];
        if(!c->occupied) {
            memset(c,0,sizeof(*c));
            memcpy(c->device.uuid,uuid,16);
        }
        const bool changed=!c->occupied || c->mesh_address!=address ||
                           !c->approved || c->rejected;
        c->occupied=true;
        c->approved=true;
        c->rejected=false;
        c->mesh_address=address;
        if(changed) {
            esp_err_t err=registry_save_locked();
            if(err!=ESP_OK)ESP_LOGE(TAG,"Device registry save failed: %s",esp_err_to_name(err));
        }
    } else ESP_LOGE(TAG,"Device registry full");
    xSemaphoreGive(lock);
    automesh_led_set_registered(true);
}

static bool valid_display_name(const char *name)
{
    if(!name)return false;
    size_t len=strlen(name);
    if(len<1 || len>NODE_NAME_BYTES)return false;
    bool visible=false;
    for(size_t i=0;i<len;i++) {
        const unsigned char c=(unsigned char)name[i];
        if(c<0x20 || c==0x7f)return false;
        if(c!=' ')visible=true;
    }
    return visible;
}
esp_err_t automesh_set_name(const char *uuid_hex,const char *name)
{
    uint8_t uuid[16];
    if(!uuid_hex || !from_hex(uuid_hex,uuid,16) || !valid_display_name(name))
        return ESP_ERR_INVALID_ARG;
    if(!lock || xSemaphoreTake(lock,pdMS_TO_TICKS(1000))!=pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err=ESP_ERR_NOT_FOUND;
    for(int i=0;i<MAX_CANDIDATES;i++) {
        candidate_t *c=&candidates[i];
        if(!c->occupied || !c->mesh_address ||
           memcmp(c->device.uuid,uuid,16))continue;
        char previous[NODE_NAME_BYTES+1];
        memcpy(previous,c->name,sizeof(previous));
        memset(c->name,0,sizeof(c->name));
        memcpy(c->name,name,strlen(name));
        err=registry_save_locked();
        if(err!=ESP_OK)memcpy(c->name,previous,sizeof(c->name));
        break;
    }
    xSemaphoreGive(lock);
    return err;
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

void automesh_note_version(uint16_t mesh_address, const uint8_t *data, size_t length)
{
    if (!lock || !data || length == 0 ||
        length >= sizeof(candidates[0].fw_version)) return;
    // Reject control and non-printable characters from the radio.
    for (size_t i=0; i<length; ++i) {
        if (data[i] < 0x20 || data[i] > 0x7e) return;
    }
    bool found=false;
    if (xSemaphoreTake(lock,pdMS_TO_TICKS(100)) != pdTRUE) return;
    for (int i=0; i<MAX_CANDIDATES; ++i) {
        candidate_t *c=&candidates[i];
        if (!c->occupied || c->mesh_address != mesh_address || !c->approved)
            continue;
        memcpy(c->fw_version, data, length);
        c->fw_version[length] = 0;
        c->last_reply_us = esp_timer_get_time();
        found=true;
        break;
    }
    xSemaphoreGive(lock);
    if (found) automesh_led_mark_alive();
}

bool automesh_mesh_address_approved(uint16_t address)
{
    if (address==0x0001) return true;
    if(!ESP_BLE_MESH_ADDR_IS_UNICAST(address) || !lock)return false;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(100))!=pdTRUE)return false;
    bool approved=false;
    for(int i=0;i<MAX_CANDIDATES;i++) {
        const candidate_t *c=&candidates[i];
        if(c->occupied && c->approved && !c->rejected && !c->removing &&
           c->mesh_address==address && provisioner_ready &&
           esp_ble_mesh_provisioner_get_node_with_uuid(c->device.uuid)) {
            approved=true;
            break;
        }
    }
    xSemaphoreGive(lock);
    return approved;
}

bool automesh_node_ota_supported(uint16_t address, uint8_t chip)
{
    if (!lock || (chip != 'C' && chip != 'S')) return false;
    const int64_t now=esp_timer_get_time();
    bool ok=false;
    if (xSemaphoreTake(lock,pdMS_TO_TICKS(150))!=pdTRUE) return false;
    for (int i=0; i<MAX_CANDIDATES; ++i) {
        const candidate_t *c=&candidates[i];
        if (!c->occupied || !c->approved || c->rejected || c->removing ||
            c->mesh_address!=address ||
            !c->last_reply_us ||
            now-c->last_reply_us>=35000000LL) continue;
        // OTA1 and OTA2 are explicitly advertised; never guess capability.
        const bool feature=strstr(c->fw_version," OTA1 ")!=NULL ||
                           strstr(c->fw_version," OTA2 ")!=NULL;
        const bool target=(chip=='C') ? strstr(c->fw_version," C3 ")!=NULL :
                                        strstr(c->fw_version," S3 ")!=NULL;
        ok=feature && target;
        break;
    }
    xSemaphoreGive(lock);
    return ok;
}

uint16_t automesh_node_ota_block_size(uint16_t address)
{
    if (!lock) return 0;
    uint16_t size=0;
    const int64_t now=esp_timer_get_time();
    if (xSemaphoreTake(lock,pdMS_TO_TICKS(150))!=pdTRUE) return 0;
    for(int i=0;i<MAX_CANDIDATES;i++) {
        const candidate_t *c=&candidates[i];
        if(!c->occupied || !c->approved || c->rejected || c->removing ||
           c->mesh_address!=address || !c->last_reply_us ||
           now-c->last_reply_us>=35000000LL)continue;
        if(strstr(c->fw_version," OTA2 "))size=AUTOMESH_NODE_OTA_BLOCK;
        else if(strstr(c->fw_version," OTA1 "))
            size=AUTOMESH_NODE_OTA_LEGACY_BLOCK;
        break;
    }
    xSemaphoreGive(lock);
    return size;
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


/* ESP-BLE-MESH provisioner node deletion is asynchronous. Only delete the
 * gateway's metadata after ESP_BLE_MESH_PROVISIONER_DELETE_NODE_WITH_UUID_COMP_EVT
 * confirms that the native provisioner database accepted the deletion.
 * This does NOT reset the remote node or revoke the shared Mesh NetKey. */
void automesh_remove_completed(const uint8_t uuid[16], esp_err_t result)
{
    if (!uuid || !lock || xSemaphoreTake(lock,pdMS_TO_TICKS(1000))!=pdTRUE)
        return;
    if (!removal_pending || memcmp(removal_uuid,uuid,16)!=0) {
        xSemaphoreGive(lock);
        return;
    }
    for (int i=0;i<MAX_CANDIDATES;i++) {
        candidate_t *c=&candidates[i];
        if (!c->occupied || memcmp(c->device.uuid,uuid,16)) continue;
        if (result==ESP_OK) {
            memset(c,0,sizeof(*c));
            esp_err_t stored=registry_save_locked();
            if (stored!=ESP_OK)
                ESP_LOGE(TAG,"Failed to persist deleted Mesh node: %s",esp_err_to_name(stored));
        } else {
            c->removing=false;
        }
        break;
    }
    removal_pending=false;
    memset(removal_uuid,0,sizeof(removal_uuid));
    xSemaphoreGive(lock);
}

esp_err_t automesh_remove_uuid(const char *uuid_hex)
{
    uint8_t uuid[16];
    if (!uuid_hex || !from_hex(uuid_hex,uuid,sizeof(uuid)) || !lock)
        return ESP_ERR_INVALID_ARG;
    if (automesh_gateway_node_ota_active()) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(lock,pdMS_TO_TICKS(1000))!=pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (removal_pending) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    int slot=-1;
    for(int i=0;i<MAX_CANDIDATES;i++) {
        const candidate_t *c=&candidates[i];
        if (c->occupied && c->approved && c->mesh_address &&
            memcmp(c->device.uuid,uuid,sizeof(uuid))==0) {
            slot=i;
            break;
        }
    }
    if(slot<0) {
        xSemaphoreGive(lock);
        return ESP_ERR_NOT_FOUND;
    }
    removal_pending=true;
    memcpy(removal_uuid,uuid,sizeof(uuid));
    candidates[slot].removing=true;
    xSemaphoreGive(lock);

    const bool native=provisioner_ready &&
        esp_ble_mesh_provisioner_get_node_with_uuid(uuid)!=NULL;
    if (!native) {
        // A legacy UI-only record can be forgotten without touching SDK keys.
        automesh_gateway_forget_node(uuid);
        automesh_remove_completed(uuid,ESP_OK);
        return ESP_OK;
    }
    esp_err_t err=esp_ble_mesh_provisioner_delete_node_with_uuid(uuid);
    if(err!=ESP_OK) automesh_remove_completed(uuid,err);
    return err;
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

esp_err_t automesh_approve_uuid(const char *uuid_hex)
{
    uint8_t uuid[16];
    if (!uuid_hex || !from_hex(uuid_hex,uuid,16)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(lock,pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_ble_mesh_unprov_dev_add_t device={0};
    int index=-1;
    for (int i=0; i<MAX_CANDIDATES; ++i) {
        if (candidates[i].occupied && candidates[i].approved &&
            candidates[i].mesh_address == 0) {
            xSemaphoreGive(lock);
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
        return ESP_ERR_NOT_FOUND;
    }
    candidates[index].approved=true;
    xSemaphoreGive(lock);

    esp_err_t err=esp_ble_mesh_provisioner_add_unprov_dev(&device,
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
    if(!out || cap<40 || !lock)return -1;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(500))!=pdTRUE)return -1;
    cJSON *root=cJSON_CreateObject();
    cJSON *array=root?cJSON_AddArrayToObject(root,"devices"):NULL;
    if(!array) {cJSON_Delete(root);xSemaphoreGive(lock);return -1;}
    int64_t now=esp_timer_get_time();
    for(int i=0;i<MAX_CANDIDATES;i++) {
        candidate_t *c=&candidates[i];
        if(!c->occupied)continue;
        char uuid[33],mac[13];
        hex_bytes(c->device.uuid,16,uuid);
        hex_bytes(c->device.addr,6,mac);
        bool alive=c->mesh_address && c->last_reply_us &&
                   (now-c->last_reply_us < 35000000LL);
        bool configured=provisioner_ready && c->mesh_address &&
            esp_ble_mesh_provisioner_get_node_with_uuid(c->device.uuid)!=NULL;
        const char *status=c->removing?"removing":c->rejected?"denied":
             c->mesh_address?(configured?(alive?"online":"offline"):"needs_repair"):
             c->approved?"provisioning":"pending";
        cJSON *item=cJSON_CreateObject();
        if(!item)break;
        cJSON_AddStringToObject(item,"uuid",uuid);
        cJSON_AddStringToObject(item,"mac",mac);
        cJSON_AddStringToObject(item,"name",c->name);
        cJSON_AddStringToObject(item,"firmware_version",c->fw_version);
        cJSON_AddStringToObject(item,"state",status);
        cJSON_AddNumberToObject(item,"address",c->mesh_address);
        cJSON_AddBoolToObject(item,"online",alive && configured);
        cJSON_AddBoolToObject(item,"mesh_configured",configured);
        cJSON_AddItemToArray(array,item);
    }
    xSemaphoreGive(lock);
    char *json=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(!json)return -1;
    size_t len=strlen(json);
    if(len>=cap){free(json);return -1;}
    memcpy(out,json,len+1);
    free(json);
    return (int)len;
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
    (void)arg;
    char line[160];
    printf("AUTOMESH {\"event\":\"ready\",\"version\":\"%s\"}\n", esp_app_get_description()->version);
    fflush(stdout);
    while (true) {
        if (!fgets(line, sizeof(line), stdin)) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        char verb[16]={0}, uuid_text[40]={0};
        int count=sscanf(line,"%15s %39s",verb,uuid_text);
        if(count==1 && strcmp(verb,"LIST")==0) {
            list_candidates();
            continue;
        }
        if(count==1 && strcmp(verb,"HELLO")==0) {
            printf("AUTOMESH {\"event\":\"ready\",\"version\":\"%s\"}\n", esp_app_get_description()->version);
            fflush(stdout);
            continue;
        }
        uint8_t uuid[16];
        if(count!=2 || !from_hex(uuid_text,uuid,sizeof(uuid))) {
            ESP_LOGW(TAG,"Invalid USB command");
            continue;
        }
        esp_err_t e=ESP_ERR_INVALID_ARG;
        if(strcmp(verb,"APPROVE")==0) {
            e=automesh_approve_uuid(uuid_text);
        } else if(strcmp(verb,"DENY")==0) {
            e=automesh_deny_uuid(uuid_text);
        }
        if(e==ESP_OK) {
            printf("AUTOMESH {\"event\":\"%s\",\"uuid\":\"%s\"}\n",
                   strcmp(verb,"APPROVE")==0?"approval_started":"denied",uuid_text);
        } else {
            printf("AUTOMESH {\"event\":\"error\",\"uuid\":\"%s\",\"reason\":\"%s\"}\n",
                   uuid_text,esp_err_to_name(e));
        }
        fflush(stdout);
    }
}

void automesh_bridge_start(void)
{
    BaseType_t ok = xTaskCreate(command_task, "automesh_usb", 4096, NULL, 5, NULL);
    if (ok != pdPASS) ESP_LOGE(TAG, "USB command task creation failed");
}



