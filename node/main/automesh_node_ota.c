#include "automesh_node_ota.h"
#include "automesh_locate.h"
#include "automesh_ota_protocol.h"
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_ble_mesh_networking_api.h"

static const char *TAG = "AUTOMESH_NODE_OTA";
typedef struct {
    bool active;
    uint16_t id;
    uint32_t size, received;
    uint8_t chip;
    uint8_t expected_sha[32];
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    mbedtls_sha256_context sha;
    int64_t touched;
} ota_session_t;
static ota_session_t ota;

// Never erase or write Flash inside the Bluetooth Mesh callback. A lengthy
// esp_ota_begin() can starve Mesh segmentation and lose the START response.
typedef struct {
    esp_ble_mesh_model_t *model;
    esp_ble_mesh_msg_ctx_t ctx;
    uint32_t opcode;
    uint16_t length;
    uint8_t data[6 + AUTOMESH_NODE_OTA_BLOCK];
} ota_message_t;
static QueueHandle_t ota_requests;
static TaskHandle_t ota_worker;

static uint8_t local_chip(void)
{
#if CONFIG_IDF_TARGET_ESP32C3
    return 'C';
#elif CONFIG_IDF_TARGET_ESP32S3
    return 'S';
#else
    return 0;
#endif
}
static void abort_session(void)
{
    if (ota.active) {
        esp_ota_abort(ota.handle);
        mbedtls_sha256_free(&ota.sha);
    }
    memset(&ota, 0, sizeof(ota));
}
static void ack(esp_ble_mesh_model_t *model, const esp_ble_mesh_msg_ctx_t *ctx,
                uint16_t id, uint32_t offset, uint8_t status)
{
    uint8_t data[7];
    automesh_put16(data, id);
    automesh_put32(data + 2, offset);
    data[6] = status;
    esp_ble_mesh_msg_ctx_t reply = *ctx;
    // Received contexts have send_ttl=0 by default. An ACK at TTL 0
    // cannot reach the gateway through relay nodes. This must be explicit.
    reply.send_ttl = 5;
    esp_err_t err = esp_ble_mesh_server_model_send_msg(
        model, &reply, AUTOMESH_NODE_OTA_STATUS, sizeof(data), data);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Status reply FAILED id=%u offset=%lu code=%u: %s",
                 (unsigned)id,(unsigned long)offset,(unsigned)status,
                 esp_err_to_name(err));
    else if (status != 0 || offset == 0)
        ESP_LOGI(TAG, "Status ACK sent id=%u offset=%lu code=%u ttl=%u",
                 (unsigned)id,(unsigned long)offset,(unsigned)status,
                 (unsigned)reply.send_ttl);
}
static void reboot_after_ota(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}
static bool image_header_matches_chip(const uint8_t *buffer, size_t length)
{
    if (length < sizeof(esp_image_header_t)) return false;
    esp_image_header_t header;
    memcpy(&header, buffer, sizeof(header));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC) return false;
#if CONFIG_IDF_TARGET_ESP32C3
    return header.chip_id == ESP_CHIP_ID_ESP32C3;
#elif CONFIG_IDF_TARGET_ESP32S3
    return header.chip_id == ESP_CHIP_ID_ESP32S3;
#else
    return false;
#endif
}

static bool process_ota_command(esp_ble_mesh_model_t *model,
                              const esp_ble_mesh_msg_ctx_t *ctx,
                              uint32_t opcode, const uint8_t *p, size_t length)
{
    if (opcode != AUTOMESH_NODE_OTA_START &&
        opcode != AUTOMESH_NODE_OTA_DATA &&
        opcode != AUTOMESH_NODE_OTA_STREAM &&
        opcode != AUTOMESH_NODE_OTA_CHECKPOINT &&
        opcode != AUTOMESH_NODE_OTA_FINISH &&
        opcode != AUTOMESH_NODE_OTA_ABORT)
        return false;
    // Model messages are authenticated by the bound Mesh AppKey;
    // additionally allow only the original gateway's unicast source.
    if (!model || !ctx || ctx->addr != 0x0001 ||
        !ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst))
        return true;
    if (!p || length < 2) return true;
    uint16_t id = automesh_u16(p);
    if (opcode == AUTOMESH_NODE_OTA_START) {
        if (length != 39) {
            ack(model, ctx, id, 0, 2);
            return true;
        }
        uint32_t size = automesh_u32(p + 2);
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (!part || size < 512 || size > AUTOMESH_NODE_OTA_MAX_IMAGE ||
            size > part->size || p[6] != local_chip()) {
            ack(model, ctx, id, 0, 2);
            return true;
        }
        if (ota.active && ota.id == id && ota.size == size &&
            ota.chip == p[6] && memcmp(ota.expected_sha, p + 7, 32) == 0) {
            // Lost START ACK: acknowledge the existing session instead of
            // replying BUSY and abandoning a valid in-progress update.
            ota.touched = esp_timer_get_time();
            ack(model, ctx, id, ota.received, 0);
            return true;
        }
        if (ota.active) {
            // A disconnected upload never replaces the running firmware.
            // A stale transfer can be abandoned after 120s of inactivity.
            if (esp_timer_get_time() - ota.touched > 120000000LL)
                abort_session();
            else {
                ack(model, ctx, id, ota.received, 1);
                return true;
            }
        }
        esp_ota_handle_t handle = 0;
        // Sequential erase avoids a long full-partition erase and keeps BLE Mesh
        // responsive. Flash operations run only on the OTA worker task.
        esp_err_t result = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
        if (result != ESP_OK) {
            ack(model, ctx, id, 0, 4);
            return true;
        }
        memset(&ota, 0, sizeof(ota));
        ota.active = true;
        ota.id = id;
        ota.size = size;
        ota.chip = p[6];
        ota.partition = part;
        ota.handle = handle;
        ota.touched = esp_timer_get_time();
        memcpy(ota.expected_sha, p + 7, 32);
        mbedtls_sha256_init(&ota.sha);
        if (mbedtls_sha256_starts(&ota.sha, 0) != 0) {
            abort_session();
            ack(model, ctx, id, 0, 4);
            return true;
        }
        ESP_LOGI(TAG, "Node OTA started (%lu bytes, session %u)",
                 (unsigned long)size, (unsigned)id);
        ack(model, ctx, id, 0, 0);
        return true;
    }
    if (!ota.active || ota.id != id) {
        ack(model, ctx, id, 0, 1);
        return true;
    }
    ota.touched = esp_timer_get_time();
    if (opcode == AUTOMESH_NODE_OTA_ABORT) {
        uint32_t offset = ota.received;
        abort_session();
        ack(model, ctx, id, offset, 0);
        return true;
    }
    if (opcode == AUTOMESH_NODE_OTA_CHECKPOINT) {
        // Replies include only Flash-confirmed contiguous bytes.
        if (length != 2) ack(model, ctx, id, ota.received, 2);
        else ack(model, ctx, id, ota.received, 0);
        return true;
    }
    if (opcode == AUTOMESH_NODE_OTA_DATA ||
        opcode == AUTOMESH_NODE_OTA_STREAM) {
        const bool streamed = opcode == AUTOMESH_NODE_OTA_STREAM;
        if (length < 7 || length > 6 + AUTOMESH_NODE_OTA_BLOCK) {
            ack(model, ctx, id, ota.received, 2);
            return true;
        }
        uint32_t offset = automesh_u32(p + 2);
        size_t count = length - 6;
        if (offset < ota.received && count <= ota.received - offset) {
            // Duplicate after a lost checkpoint; Flash is already committed.
            // Do not acknowledge individual OTA3 stream packets.
            if (!streamed) ack(model, ctx, id, ota.received, 0);
            return true;
        }
        if (offset != ota.received || count > ota.size - ota.received ||
            (offset == 0 && !image_header_matches_chip(p + 6, count))) {
            // OTA3 can recover a missing packet at the next checkpoint.
            // Legacy DATA retains the original explicit error.
            if (!streamed) ack(model, ctx, id, ota.received, 2);
            return true;
        }
        if (esp_ota_write(ota.handle, p + 6, count) != ESP_OK ||
            mbedtls_sha256_update(&ota.sha, p + 6, count) != 0) {
            uint32_t previous = ota.received;
            abort_session();
            ack(model, ctx, id, previous, 4);
            return true;
        }
        ota.received += count;
        if (!streamed) ack(model, ctx, id, ota.received, 0);
        return true;
    }
    if (opcode == AUTOMESH_NODE_OTA_FINISH) {
        if (length != 2 || ota.received != ota.size) {
            ack(model, ctx, id, ota.received, 2);
            return true;
        }
        uint8_t actual[32];
        uint32_t full_size = ota.received;
        if (mbedtls_sha256_finish(&ota.sha, actual) != 0 ||
            memcmp(actual, ota.expected_sha, 32) != 0) {
            abort_session();
            ack(model, ctx, id, full_size, 3);
            return true;
        }
        esp_err_t e = esp_ota_end(ota.handle);
        // esp_ota_end closes the handle even on failure.
        ota.active = false;
        mbedtls_sha256_free(&ota.sha);
        if (e != ESP_OK) {
            memset(&ota, 0, sizeof(ota));
            ack(model, ctx, id, full_size, 3);
            return true;
        }
        esp_app_desc_t app = {0};
        e = esp_ota_get_partition_description(ota.partition, &app);
        if (e != ESP_OK || strcmp(app.project_name, "onoff_server") != 0) {
            memset(&ota, 0, sizeof(ota));
            ack(model, ctx, id, full_size, 3);
            return true;
        }
        e = esp_ota_set_boot_partition(ota.partition);
        memset(&ota, 0, sizeof(ota));
        ack(model, ctx, id, full_size, e == ESP_OK ? 0 : 4);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "Node OTA validated. Reboot scheduled.");
            xTaskCreate(reboot_after_ota, "node_ota_restart", 2048, NULL, 4, NULL);
        }
        return true;
    }
    return true;
}

static void ota_worker_task(void *argument)
{
    (void)argument;
    ota_message_t message;
    while (true) {
        if (xQueueReceive(ota_requests, &message, portMAX_DELAY) != pdTRUE)
            continue;
        (void)process_ota_command(message.model, &message.ctx,
                                  message.opcode, message.data, message.length);
    }
}

esp_err_t automesh_node_ota_init(void)
{
    if (ota_requests) return ESP_OK;
    ota_requests = xQueueCreate(8, sizeof(ota_message_t));
    if (!ota_requests) return ESP_ERR_NO_MEM;
    if (xTaskCreate(ota_worker_task, "node_mesh_ota", 5120,
                    NULL, 5, &ota_worker) != pdPASS) {
        vQueueDelete(ota_requests);
        ota_requests = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Mesh OTA worker ready: Flash operations outside BLE callback");
    return ESP_OK;
}

bool automesh_node_ota_command(esp_ble_mesh_model_t *model,
                              const esp_ble_mesh_msg_ctx_t *ctx,
                              uint32_t opcode, const uint8_t *payload,
                              size_t length)
{
    if (opcode != AUTOMESH_NODE_OTA_START &&
        opcode != AUTOMESH_NODE_OTA_DATA &&
        opcode != AUTOMESH_NODE_OTA_STREAM &&
        opcode != AUTOMESH_NODE_OTA_CHECKPOINT &&
        opcode != AUTOMESH_NODE_OTA_FINISH &&
        opcode != AUTOMESH_NODE_OTA_ABORT)
        return false;
    if (!model || !ctx || ctx->addr != 0x0001 ||
        !ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst))
        return true;
    if (!payload || length < 2 ||
        length > 6 + AUTOMESH_NODE_OTA_BLOCK || !ota_requests) {
        ESP_LOGW(TAG, "Invalid OTA opcode=0x%08lx, length=%u, queue=%d",
                 (unsigned long)opcode, (unsigned)length, !!ota_requests);
        return true;
    }
    ota_message_t message = {.model = model, .ctx = *ctx,
                             .opcode = opcode, .length = (uint16_t)length};
    memcpy(message.data, payload, length);
    if (xQueueSend(ota_requests, &message, 0) != pdTRUE) {
        ESP_LOGW(TAG, "OTA worker busy; rejecting opcode 0x%08lx",
                 (unsigned long)opcode);
        // Reject rather than hanging the sending gateway forever.
        ack(model, ctx, automesh_u16(payload), ota.received, 1);
    }
    return true;
}
