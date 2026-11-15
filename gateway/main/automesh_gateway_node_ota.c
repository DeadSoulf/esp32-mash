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
    bool active, awaiting, allow_short_ack, stream, send_pending;
    uint16_t address, id;
    uint32_t size, sent, expected_ack, ack_offset;
    uint8_t ack_status;
    esp_err_t send_result;
    uint8_t chip;
    uint16_t chunk_len;   // Negotiated OTA1=96 or OTA2=224; never inferred
} ota_state_t;

static esp_ble_mesh_model_t *mesh_model;
static uint16_t mesh_net_idx, mesh_app_idx;
static SemaphoreHandle_t reply_event;
static SemaphoreHandle_t stream_event;
static portMUX_TYPE state_guard = portMUX_INITIALIZER_UNLOCKED;
static ota_state_t transfer;

void automesh_gateway_node_ota_init(esp_ble_mesh_model_t *model,
                                    uint16_t net_idx, uint16_t app_idx)
{
    mesh_model = model;
    mesh_net_idx = net_idx;
    mesh_app_idx = app_idx;
    if (!reply_event) reply_event = xSemaphoreCreateBinary();
    if (!stream_event) stream_event = xSemaphoreCreateBinary();
    configASSERT(reply_event != NULL && stream_event != NULL);
}
bool automesh_gateway_node_ota_active(void)
{
    bool active;
    portENTER_CRITICAL(&state_guard);
    active=transfer.active;
    portEXIT_CRITICAL(&state_guard);
    return active;
}
uint16_t automesh_gateway_node_ota_max_chunk(void)
{
    uint16_t n=0;
    portENTER_CRITICAL(&state_guard);
    if(transfer.active)n=transfer.chunk_len;
    portEXIT_CRITICAL(&state_guard);
    return n;
}
bool automesh_gateway_node_ota_streaming(void)
{
    bool stream;
    portENTER_CRITICAL(&state_guard);
    stream=transfer.active && transfer.stream;
    portEXIT_CRITICAL(&state_guard);
    return stream;
}

void automesh_gateway_node_ota_on_send_complete(uint32_t opcode, esp_err_t result)
{
    if (opcode!=AUTOMESH_NODE_OTA_STREAM || !stream_event)return;
    bool notify=false;
    portENTER_CRITICAL(&state_guard);
    if (transfer.active && transfer.stream && transfer.send_pending) {
        transfer.send_result=result;
        transfer.send_pending=false;
        notify=true;
    }
    portEXIT_CRITICAL(&state_guard);
    if (notify) xSemaphoreGive(stream_event);
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
        (offset==transfer.expected_ack ||
         (transfer.allow_short_ack && offset>=transfer.sent &&
          offset<=transfer.expected_ack) || payload[6]!=0)) {
        transfer.ack_offset=offset;
        transfer.ack_status=payload[6];
        transfer.awaiting=false;
        accept=true;
    }
    portEXIT_CRITICAL(&state_guard);
    if (accept) xSemaphoreGive(reply_event);
}

static const char *ota_stage(uint32_t opcode)
{
    if (opcode == AUTOMESH_NODE_OTA_START) return "start";
    if (opcode == AUTOMESH_NODE_OTA_DATA) return "data";
    if (opcode == AUTOMESH_NODE_OTA_CHECKPOINT) return "checkpoint";
    if (opcode == AUTOMESH_NODE_OTA_STREAM) return "stream";
    if (opcode == AUTOMESH_NODE_OTA_FINISH) return "finish";
    if (opcode == AUTOMESH_NODE_OTA_ABORT) return "abort";
    return "unknown";
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
    portEXIT_CRITICAL(&state_guard);

    // Lost Mesh acknowledgements are normal on a busy radio network.
    // START and DATA are idempotent on the node; FINISH is intentionally
    // never repeated because the node may already have rebooted.
    const unsigned attempts = (opcode==AUTOMESH_NODE_OTA_START ||
                               opcode==AUTOMESH_NODE_OTA_DATA ||
                               opcode==AUTOMESH_NODE_OTA_CHECKPOINT) ? 3u : 1u;
    const uint32_t timeout_ms=opcode==AUTOMESH_NODE_OTA_START?45000u:
                              opcode==AUTOMESH_NODE_OTA_FINISH?30000u:
                              (opcode==AUTOMESH_NODE_OTA_DATA ||
                               opcode==AUTOMESH_NODE_OTA_CHECKPOINT)?15000u:8500u;

    for (unsigned attempt=1; attempt<=attempts; ++attempt) {
        portENTER_CRITICAL(&state_guard);
        if (!transfer.active) {
            portEXIT_CRITICAL(&state_guard);
            return ESP_ERR_INVALID_STATE;
        }
        portEXIT_CRITICAL(&state_guard);

        // Drain the previous attempt BEFORE arming this one. Otherwise a
        // late STATUS can disarm transfer.awaiting and drop the new ACK.
        while (xSemaphoreTake(reply_event, 0) == pdTRUE) {}
        portENTER_CRITICAL(&state_guard);
        transfer.expected_ack=expected_offset;
        transfer.allow_short_ack=(opcode==AUTOMESH_NODE_OTA_CHECKPOINT);
        transfer.awaiting=true;
        portEXIT_CRITICAL(&state_guard);

        esp_ble_mesh_msg_ctx_t ctx={0};
        ctx.net_idx=mesh_net_idx;
        ctx.app_idx=mesh_app_idx;
        ctx.addr=addr;
        ctx.send_ttl=5;
        esp_err_t err=esp_ble_mesh_client_model_send_msg(mesh_model, &ctx, opcode,
            (uint16_t)len, (uint8_t *)payload, 0, false, ROLE_PROVISIONER);
        if(err!=ESP_OK) {
            portENTER_CRITICAL(&state_guard);
            transfer.awaiting=false;
            portEXIT_CRITICAL(&state_guard);
            ESP_LOGW(TAG,"Mesh OTA send error stage=%s node=0x%04x offset=%lu attempt=%u: %s",
                     ota_stage(opcode),addr,(unsigned long)expected_offset,
                     attempt,esp_err_to_name(err));
            if(attempt==attempts)return err;
        } else if (xSemaphoreTake(reply_event, pdMS_TO_TICKS(timeout_ms))==pdTRUE) {
            uint8_t status;
            uint32_t actual;
            portENTER_CRITICAL(&state_guard);
            status=transfer.ack_status;
            actual=transfer.ack_offset;
            portEXIT_CRITICAL(&state_guard);
            if(status!=0) {
                ESP_LOGW(TAG,"Mesh OTA NACK stage=%s node=0x%04x offset=%lu status=%u",
                         ota_stage(opcode),addr,(unsigned long)actual,(unsigned)status);
                return status==1 ? ESP_ERR_INVALID_STATE :
                       status==2 ? ESP_ERR_INVALID_ARG : ESP_FAIL;
            }
            if (opcode != AUTOMESH_NODE_OTA_DATA)
                ESP_LOGI(TAG,"Mesh OTA ACK stage=%s node=0x%04x offset=%lu",
                         ota_stage(opcode),addr,(unsigned long)actual);
            return ESP_OK;
        } else {
            portENTER_CRITICAL(&state_guard);
            transfer.awaiting=false;
            portEXIT_CRITICAL(&state_guard);
            ESP_LOGW(TAG,"Mesh OTA ACK TIMEOUT stage=%s node=0x%04x offset=%lu "
                         "attempt=%u/%u waited=%lums",
                     ota_stage(opcode),addr,(unsigned long)expected_offset,
                     attempt,attempts,(unsigned long)timeout_ms);
            if(attempt==attempts)return ESP_ERR_TIMEOUT;
        }
        // Give the segmented transport a short recovery interval.
        vTaskDelay(pdMS_TO_TICKS(250u*attempt));
    }
    return ESP_ERR_TIMEOUT;
}

/* Completion is lower-transport send completion, not application ACK.
 * At most one segmented STREAM is active at any time. The node confirms
 * the contiguous Flash-written offset separately with CHECKPOINT.
 */
static esp_err_t stream_send(uint32_t offset,const uint8_t *bytes,size_t length)
{
    if(!stream_event || !mesh_model || !bytes ||
       length==0 || length>AUTOMESH_NODE_OTA_BLOCK)
        return ESP_ERR_INVALID_ARG;
    uint8_t packet[6+AUTOMESH_NODE_OTA_BLOCK];
    uint16_t id,addr;
    portENTER_CRITICAL(&state_guard);
    if(!transfer.active || !transfer.stream || transfer.send_pending ||
       transfer.awaiting) {
        portEXIT_CRITICAL(&state_guard);
        return ESP_ERR_INVALID_STATE;
    }
    id=transfer.id;
    addr=transfer.address;
    // Consume the previous send completion before arming a new packet.
    while(xSemaphoreTake(stream_event,0)==pdTRUE){}
    transfer.send_pending=true;
    transfer.send_result=ESP_FAIL;
    portEXIT_CRITICAL(&state_guard);
    automesh_put16(packet,id);
    automesh_put32(packet+2,offset);
    memcpy(packet+6,bytes,length);
    esp_ble_mesh_msg_ctx_t ctx={0};
    ctx.net_idx=mesh_net_idx;
    ctx.app_idx=mesh_app_idx;
    ctx.addr=addr;
    ctx.send_ttl=5;
    esp_err_t result=esp_ble_mesh_client_model_send_msg(mesh_model,&ctx,
        AUTOMESH_NODE_OTA_STREAM,(uint16_t)(6+length),packet,
        0,false,ROLE_PROVISIONER);
    if(result!=ESP_OK) {
        portENTER_CRITICAL(&state_guard);
        transfer.send_pending=false;
        portEXIT_CRITICAL(&state_guard);
        return result;
    }
    if(xSemaphoreTake(stream_event,pdMS_TO_TICKS(20000))!=pdTRUE) {
        portENTER_CRITICAL(&state_guard);
        transfer.send_pending=false;
        portEXIT_CRITICAL(&state_guard);
        ESP_LOGW(TAG,"OTA3 Mesh SEND completion timeout at offset %lu",
                 (unsigned long)offset);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&state_guard);
    result=transfer.send_result;
    portEXIT_CRITICAL(&state_guard);
    return result;
}

esp_err_t automesh_gateway_node_ota_batch(uint32_t offset,const uint8_t *data,
                                          size_t len,uint32_t *confirmed)
{
    if(!data || !confirmed || len==0 ||
       len>AUTOMESH_NODE_OTA_BLOCK*AUTOMESH_NODE_OTA_STREAM_WINDOW)
        return ESP_ERR_INVALID_ARG;
    if(!transfer.active)return ESP_ERR_INVALID_STATE;
    if(!transfer.stream) {
        uint32_t pos=offset;
        size_t used=0;
        while(used<len) {
            size_t n=len-used<transfer.chunk_len?len-used:transfer.chunk_len;
            esp_err_t e=automesh_gateway_node_ota_chunk(pos,data+used,n,confirmed);
            if(e!=ESP_OK)return e;
            pos+=(uint32_t)n;
            used+=n;
        }
        return ESP_OK;
    }
    if(offset!=transfer.sent || len>transfer.size-transfer.sent)
        return ESP_ERR_INVALID_ARG;
    const uint32_t goal=offset+(uint32_t)len;
    uint32_t resume=offset;
    // Missing segmented messages are retransmitted from the first missing
    // contiguous byte, never by blindly replaying a completed image.
    for(unsigned round=0;round<4 && resume<goal;round++) {
        for(uint32_t pos=resume;pos<goal;) {
            size_t count=(goal-pos)<transfer.chunk_len?
                         (size_t)(goal-pos):transfer.chunk_len;
            esp_err_t e=stream_send(pos,data+(pos-offset),count);
            if(e!=ESP_OK)return e;
            pos+=(uint32_t)count;
        }
        uint8_t query[2];
        automesh_put16(query,transfer.id);
        esp_err_t e=send_await(AUTOMESH_NODE_OTA_CHECKPOINT,query,sizeof(query),goal);
        if(e!=ESP_OK)return e;
        uint32_t received;
        portENTER_CRITICAL(&state_guard);
        received=transfer.ack_offset;
        portEXIT_CRITICAL(&state_guard);
        if(received<resume || received>goal)return ESP_FAIL;
        if(received==resume)
            ESP_LOGW(TAG,"OTA3 checkpoint no progress, retry %u/4 at %lu",
                     round+1,(unsigned long)resume);
        resume=received;
        portENTER_CRITICAL(&state_guard);
        transfer.sent=resume;
        portEXIT_CRITICAL(&state_guard);
    }
    *confirmed=resume;
    return resume==goal?ESP_OK:ESP_ERR_TIMEOUT;
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
    const uint16_t max_chunk=automesh_node_ota_block_size(address);
    const bool use_stream=automesh_node_ota_stream_supported(address);
    if (max_chunk!=AUTOMESH_NODE_OTA_LEGACY_BLOCK &&
        max_chunk!=AUTOMESH_NODE_OTA_BLOCK) return ESP_ERR_NOT_SUPPORTED;
    const uint16_t proposed_id=(uint16_t)(esp_random()|1u);
    portENTER_CRITICAL(&state_guard);
    if (transfer.active) {
        portEXIT_CRITICAL(&state_guard);
        return ESP_ERR_INVALID_STATE;
    }
    transfer=(ota_state_t){.active=true,.address=address,
        .id=proposed_id,.chip=chip,.size=image_size,.chunk_len=max_chunk,
        .stream=use_stream};
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
    ESP_LOGI(TAG,"Node OTA accepted by 0x%04x; %lu bytes, max DATA block=%u, protocol=%s",
             address,(unsigned long)image_size,(unsigned)max_chunk,
             use_stream?"OTA3":"OTA1/OTA2");
    return ESP_OK;
}

esp_err_t automesh_gateway_node_ota_chunk(uint32_t offset, const uint8_t *data,
                                          size_t len, uint32_t *confirmed)
{
    if (!data || !confirmed || len < 1 || len > AUTOMESH_NODE_OTA_BLOCK)
        return ESP_ERR_INVALID_ARG;
    if (!transfer.active) return ESP_ERR_INVALID_STATE;
    // Never send a larger DATA message to an installed OTA1-only node.
    if(len>transfer.chunk_len) return ESP_ERR_INVALID_ARG;
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
