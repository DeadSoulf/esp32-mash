#include "automesh_gateway_node_ota.h"
#include "automesh_locate.h"
#include "automesh_ota_protocol.h"
#include "automesh_bridge.h"
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG="MESH_NODE_OTA";
typedef struct {
    bool active, awaiting;
    uint16_t address, id;
    uint32_t size, sent, expected_ack, ack_offset;
    uint8_t ack_status;
    uint8_t chip;
} ota_state_t;

static esp_ble_mesh_model_t *mesh_model;
static uint16_t mesh_net_idx, mesh_app_idx;
static SemaphoreHandle_t reply_event;
static portMUX_TYPE state_guard = portMUX_INITIALIZER_UNLOCKED;
static ota_state_t transfer;

void automesh_gateway_node_ota_init(esp_ble_mesh_model_t *model,
                                    uint16_t net_idx, uint16_t app_idx)
{
    mesh_model = model;
    mesh_net_idx = net_idx;
    mesh_app_idx = app_idx;
    if (!reply_event) reply_event = xSemaphoreCreateBinary();
    configASSERT(reply_event != NULL);
}
bool automesh_gateway_node_ota_active(void)
{
    bool active;
    portENTER_CRITICAL(&state_guard);
    active=transfer.active;
    portEXIT_CRITICAL(&state_guard);
    return active;
}
uint32_t automesh_gateway_node_ota_progress(void)
{
    uint32_t n;
    portENTER_CRITICAL(&state_guard);
    n=transfer.sent;
    portEXIT_CRITICAL(&state_guard);
    return n;
}

void automesh_gateway_node_ota_on_status(uint16_t source,
                                         const uint8_t *payload, size_t length)
{
    if (!payload || length != 7 || !reply_event) return;
    const uint16_t id=automesh_u16(payload);
    const uint32_t offset=automesh_u32(payload + 2);
    bool accept=false;
    portENTER_CRITICAL(&state_guard);
    if (transfer.active && transfer.awaiting &&
        transfer.address==source && transfer.id==id &&
        (offset==transfer.expected_ack || payload[6]!=0)) {
        transfer.ack_offset=offset;
        transfer.ack_status=payload[6];
        transfer.awaiting=false;
        accept=true;
    }
    portEXIT_CRITICAL(&state_guard);
    if (accept) xSemaphoreGive(reply_event);
}

static esp_err_t send_await(uint32_t opcode, const uint8_t *payload,
                            size_t len, uint32_t expected_offset)
{
    if (!mesh_model || !reply_event) return ESP_ERR_INVALID_STATE;
    uint16_t addr;
    portENTER_CRITICAL(&state_guard);
    if (!transfer.active || transfer.awaiting) {
        portEXIT_CRITICAL(&state_guard);
        return ESP_ERR_INVALID_STATE;
    }
    addr=transfer.address;
    transfer.expected_ack=expected_offset;
    transfer.awaiting=true;
    portEXIT_CRITICAL(&state_guard);
    // Drain a late ACK from an earlier attempt.
    while (xSemaphoreTake(reply_event, 0) == pdTRUE) {}
    esp_ble_mesh_msg_ctx_t ctx={0};
    ctx.net_idx=mesh_net_idx;
    ctx.app_idx=mesh_app_idx;
    ctx.addr=addr;
    ctx.send_ttl=5;
    esp_err_t err=esp_ble_mesh_client_model_send_msg(mesh_model, &ctx, opcode,
        (uint16_t)len, (uint8_t *)payload, 0, false, ROLE_PROVISIONER);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&state_guard);
        transfer.awaiting=false;
        portEXIT_CRITICAL(&state_guard);
        return err;
    }
    // ESP flash erase on START and final image verification can take longer
    // than a normal Mesh chunk acknowledgement.
    const uint32_t timeout_ms=opcode==AUTOMESH_NODE_OTA_START?45000u:
                              opcode==AUTOMESH_NODE_OTA_FINISH?30000u:8500u;
    if (xSemaphoreTake(reply_event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        portENTER_CRITICAL(&state_guard);
        transfer.awaiting=false;
        portEXIT_CRITICAL(&state_guard);
        return ESP_ERR_TIMEOUT;
    }
    uint8_t status;
    portENTER_CRITICAL(&state_guard);
    status=transfer.ack_status;
    portEXIT_CRITICAL(&state_guard);
    if (status != 0) {
        ESP_LOGW(TAG, "Node %04x refused OTA cmd (reason %u)",
                 addr, (unsigned)status);
        return (status == 1) ? ESP_ERR_INVALID_STATE :
               (status == 2) ? ESP_ERR_INVALID_ARG : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t automesh_gateway_node_ota_start(uint16_t address, uint8_t chip,
                                          uint32_t image_size, const uint8_t sha[32],
                                          uint16_t *session)
{
    if (!mesh_model || !reply_event || !sha || !session ||
        !ESP_BLE_MESH_ADDR_IS_UNICAST(address) ||
        (chip != 'C' && chip != 'S') || image_size<512 ||
        image_size>AUTOMESH_NODE_OTA_MAX_IMAGE ||
        !esp_ble_mesh_provisioner_get_node_with_addr(address))
        return ESP_ERR_INVALID_ARG;
    // Never send experimental OTA opcodes to unknown, offline or old firmware.
    if (!automesh_node_ota_supported(address, chip))
        return ESP_ERR_NOT_SUPPORTED;
    const uint16_t proposed_id=(uint16_t)(esp_random()|1u);
    portENTER_CRITICAL(&state_guard);
    if (transfer.active) {
        portEXIT_CRITICAL(&state_guard);
        return ESP_ERR_INVALID_STATE;
    }
    transfer=(ota_state_t){.active=true,.address=address,
        .id=proposed_id,.chip=chip,.size=image_size};
    portEXIT_CRITICAL(&state_guard);
    uint8_t packet[39];
    automesh_put16(packet, transfer.id);
    automesh_put32(packet + 2, image_size);
    packet[6]=chip;
    memcpy(packet + 7, sha, 32);
    esp_err_t err=send_await(AUTOMESH_NODE_OTA_START, packet, sizeof(packet), 0);
    if (err != ESP_OK) {
        // Attempt a best-effort abort if the command may have reached the node.
        uint8_t stop[2]; automesh_put16(stop,transfer.id);
        (void)send_await(AUTOMESH_NODE_OTA_ABORT,stop,sizeof(stop),0);
        portENTER_CRITICAL(&state_guard);
        memset(&transfer,0,sizeof(transfer));
        portEXIT_CRITICAL(&state_guard);
        return err;
    }
    *session=transfer.id;
    ESP_LOGI(TAG,"Node OTA accepted by 0x%04x; %lu bytes",address,(unsigned long)image_size);
    return ESP_OK;
}

esp_err_t automesh_gateway_node_ota_chunk(uint32_t offset, const uint8_t *data,
                                          size_t len, uint32_t *confirmed)
{
    if (!data || !confirmed || len < 1 || len > AUTOMESH_NODE_OTA_BLOCK)
        return ESP_ERR_INVALID_ARG;
    if (!transfer.active) return ESP_ERR_INVALID_STATE;
    // Retry of a single request whose HTTP reply was lost.
    if (offset < transfer.sent && offset + len == transfer.sent) {
        *confirmed=transfer.sent;
        return ESP_OK;
    }
    if (offset != transfer.sent || len > transfer.size - transfer.sent)
        return ESP_ERR_INVALID_ARG;
    uint8_t packet[6+AUTOMESH_NODE_OTA_BLOCK];
    automesh_put16(packet, transfer.id);
    automesh_put32(packet + 2, offset);
    memcpy(packet + 6, data, len);
    esp_err_t err=send_await(AUTOMESH_NODE_OTA_DATA,packet,6+len,
                             offset+(uint32_t)len);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&state_guard);
        transfer.sent=offset+(uint32_t)len;
        portEXIT_CRITICAL(&state_guard);
    }
    *confirmed=transfer.sent;
    return err;
}

esp_err_t automesh_gateway_node_ota_finish(void)
{
    if (!transfer.active || transfer.sent != transfer.size)
        return ESP_ERR_INVALID_STATE;
    uint8_t packet[2];automesh_put16(packet,transfer.id);
    esp_err_t err=send_await(AUTOMESH_NODE_OTA_FINISH,packet,2,transfer.size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,"Node OTA finished; node reboot pending");
        portENTER_CRITICAL(&state_guard);
        memset(&transfer,0,sizeof(transfer));
        portEXIT_CRITICAL(&state_guard);
    }
    return err;
}
esp_err_t automesh_gateway_node_ota_abort(void)
{
    if (!transfer.active) return ESP_OK;
    uint8_t packet[2];automesh_put16(packet,transfer.id);
    esp_err_t err=send_await(AUTOMESH_NODE_OTA_ABORT,packet,2,transfer.sent);
    portENTER_CRITICAL(&state_guard);
    memset(&transfer,0,sizeof(transfer));
    portEXIT_CRITICAL(&state_guard);
    return err;
}
