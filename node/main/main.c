/* main.c - Application main entry point */

/*
 * SPDX-FileCopyrightText: 2017 Intel Corporation
 * SPDX-FileContributor: 2018-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_random.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"

#include "board.h"
#include "ble_mesh_example_init.h"
#include "automesh_status_led.h"
#include "automesh_locate.h"
#include "automesh_node_ota.h"
#include "automesh_ota_protocol.h"
#include "esp_timer.h"

#define TAG "EXAMPLE"

#define CID_ESP 0x02E5

extern struct _led_state led_state[3];

static uint8_t dev_uuid[16] = { 0xdd, 0xdd };

static esp_ble_mesh_cfg_srv_t config_server = {
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_0, 2 + 3, ROLE_NODE);
static esp_ble_mesh_gen_onoff_srv_t onoff_server_0 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_RSP_BY_APP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_1, 2 + 3, ROLE_NODE);
static esp_ble_mesh_gen_onoff_srv_t onoff_server_1 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_RSP_BY_APP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_RSP_BY_APP,
    },
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_2, 2 + 3, ROLE_NODE);
static esp_ble_mesh_gen_onoff_srv_t onoff_server_2 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_RSP_BY_APP,
    },
};

static esp_ble_mesh_model_op_t locate_op[] = {
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_LOCATE_REQ, 3),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_FW_VERSION_GET, 0),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_NODE_OTA_START, 39),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_NODE_OTA_DATA, 7),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_NODE_OTA_FINISH, 2),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_NODE_OTA_ABORT, 2),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_PEER_HELLO, 2),
    ESP_BLE_MESH_MODEL_OP_END,
};
static esp_ble_mesh_model_t locate_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(AUTOMESH_VENDOR_CID, AUTOMESH_LOCATE_SRV_MODEL,
                              locate_op, NULL, NULL),
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_0, &onoff_server_0),
};

static esp_ble_mesh_model_t extend_model_0[] = {
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_1, &onoff_server_1),
};

static esp_ble_mesh_model_t extend_model_1[] = {
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_2, &onoff_server_2),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, locate_models),
    ESP_BLE_MESH_ELEMENT(0, extend_model_0, ESP_BLE_MESH_MODEL_NONE),
    ESP_BLE_MESH_ELEMENT(0, extend_model_1, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

/* Pairing is initiated only by an authenticated gateway administrator.
 * Bluetooth Mesh uses No OOB provisioning: no second device code or key is
 * requested. This does not authenticate physical device identity against an
 * active radio attacker; use only in trusted commissioning conditions.
 */
#define MAX_MESH_PEERS 48
typedef struct {
    uint16_t address;
    int64_t seen_us;
} mesh_peer_t;
static mesh_peer_t mesh_peers[MAX_MESH_PEERS];
static portMUX_TYPE mesh_peers_guard=portMUX_INITIALIZER_UNLOCKED;

static void automesh_note_peer(uint16_t address)
{
    if (!ESP_BLE_MESH_ADDR_IS_UNICAST(address) ||
        address == esp_ble_mesh_get_primary_element_address()) return;
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&mesh_peers_guard);
    int slot=-1, oldest=0;
    for (int i=0;i<MAX_MESH_PEERS;i++) {
        if (mesh_peers[i].address==address) {slot=i;break;}
        if (mesh_peers[i].address==0 && slot<0) slot=i;
        if (mesh_peers[i].seen_us<mesh_peers[oldest].seen_us) oldest=i;
    }
    if (slot<0) slot=oldest;
    mesh_peers[slot]=(mesh_peer_t){.address=address,.seen_us=now};
    portEXIT_CRITICAL(&mesh_peers_guard);
    // The status light reflects contact with ANY trusted Mesh peer,
    // not only the gateway. Losing the gateway does not erase Mesh keys.
    automesh_led_mark_alive();
}
static void automesh_peer_task(void *arg)
{
    (void)arg;
    uint16_t seq=0;
    while (true) {
        if (esp_ble_mesh_node_is_provisioned() &&
            esp_ble_mesh_get_primary_element_address()!=ESP_BLE_MESH_ADDR_UNASSIGNED) {
            esp_ble_mesh_msg_ctx_t ctx={0};
            ctx.net_idx=ESP_BLE_MESH_KEY_PRIMARY;
            ctx.app_idx=0x0000; // Gateway binds this AppKey to the node's vendor model
            ctx.addr=ESP_BLE_MESH_ADDR_ALL_NODES;
            ctx.send_ttl=4;
            uint8_t payload[2];
            automesh_put16(payload,++seq);
            esp_err_t err=esp_ble_mesh_server_model_send_msg(
                &locate_models[0],&ctx,AUTOMESH_PEER_HELLO,sizeof(payload),payload);
            if(err!=ESP_OK)
                ESP_LOGD(TAG,"Peer hello skipped (model not yet bound): %s",esp_err_to_name(err));
            int64_t now=esp_timer_get_time();
            unsigned live=0;
            portENTER_CRITICAL(&mesh_peers_guard);
            for(int i=0;i<MAX_MESH_PEERS;i++)
                if(mesh_peers[i].address &&
                   now-mesh_peers[i].seen_us<180000000LL)live++;
            portEXIT_CRITICAL(&mesh_peers_guard);
            ESP_LOGI(TAG,"Mesh peer-to-peer: %u nearby peers (last 180s)",live);
        }
        // Low-rate AppKey-protected heartbeat; Mesh relay stays independent.
        vTaskDelay(pdMS_TO_TICKS(60000u+(esp_random()%12000u)));
    }
}

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
    .static_val = NULL,
    .static_val_len = 0,
    .output_size = 0,
    .output_actions = 0,
};

static void prov_complete(uint16_t net_idx, uint16_t addr, uint8_t flags, uint32_t iv_index)
{
    ESP_LOGI(TAG, "net_idx: 0x%04x, addr: 0x%04x", net_idx, addr);
    ESP_LOGI(TAG, "flags: 0x%02x, iv_index: 0x%08" PRIx32, flags, iv_index);
    automesh_led_set_registered(true);
}

static void example_change_led_state(esp_ble_mesh_model_t *model,
                                     esp_ble_mesh_msg_ctx_t *ctx, uint8_t onoff)
{
    uint16_t primary_addr = esp_ble_mesh_get_primary_element_address();
    uint8_t elem_count = esp_ble_mesh_get_element_count();
    struct _led_state *led = NULL;
    uint8_t i;

    if (ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst)) {
        for (i = 0; i < elem_count; i++) {
            if (ctx->recv_dst == (primary_addr + i)) {
                led = &led_state[i];
                board_led_operation(led->pin, onoff);
            }
        }
    } else if (ESP_BLE_MESH_ADDR_IS_GROUP(ctx->recv_dst)) {
        if (esp_ble_mesh_is_model_subscribed_to_group(model, ctx->recv_dst)) {
            led = &led_state[model->element->element_addr - primary_addr];
            board_led_operation(led->pin, onoff);
        }
    } else if (ctx->recv_dst == 0xFFFF) {
        led = &led_state[model->element->element_addr - primary_addr];
        board_led_operation(led->pin, onoff);
    }
}

static void example_handle_gen_onoff_msg(esp_ble_mesh_model_t *model,
                                         esp_ble_mesh_msg_ctx_t *ctx,
                                         esp_ble_mesh_server_recv_gen_onoff_set_t *set)
{
    esp_ble_mesh_gen_onoff_srv_t *srv = (esp_ble_mesh_gen_onoff_srv_t *)model->user_data;

    switch (ctx->recv_op) {
    case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET:
        esp_ble_mesh_server_model_send_msg(model, ctx,
            ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS, sizeof(srv->state.onoff), &srv->state.onoff);
        break;
    case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET:
    case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK:
        if (set->op_en == false) {
            srv->state.onoff = set->onoff;
        } else {
            /* TODO: Delay and state transition */
            srv->state.onoff = set->onoff;
        }
        if (ctx->recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
            esp_ble_mesh_server_model_send_msg(model, ctx,
                ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS, sizeof(srv->state.onoff), &srv->state.onoff);
        }
        esp_ble_mesh_model_publish(model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS,
            sizeof(srv->state.onoff), &srv->state.onoff, ROLE_NODE);
        example_change_led_state(model, ctx, srv->state.onoff);
        break;
    default:
        break;
    }
}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d", param->prov_register_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT, err_code %d", param->node_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT, bearer %s",
            param->node_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT, bearer %s",
            param->node_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT");
        prov_complete(param->node_prov_complete.net_idx, param->node_prov_complete.addr,
            param->node_prov_complete.flags, param->node_prov_complete.iv_index);
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_RESET_EVT");
        automesh_led_set_registered(false);
        break;
    case ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT, err_code %d", param->node_set_unprov_dev_name_comp.err_code);
        break;
    default:
        break;
    }
}

static void example_ble_mesh_generic_server_cb(esp_ble_mesh_generic_server_cb_event_t event,
                                               esp_ble_mesh_generic_server_cb_param_t *param)
{
    esp_ble_mesh_gen_onoff_srv_t *srv;
    ESP_LOGI(TAG, "event 0x%02x, opcode 0x%04" PRIx32 ", src 0x%04x, dst 0x%04x",
        event, param->ctx.recv_op, param->ctx.addr, param->ctx.recv_dst);

    switch (event) {
    case ESP_BLE_MESH_GENERIC_SERVER_STATE_CHANGE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_SERVER_STATE_CHANGE_EVT");
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
            param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
            ESP_LOGI(TAG, "onoff 0x%02x", param->value.state_change.onoff_set.onoff);
            example_change_led_state(param->model, &param->ctx, param->value.state_change.onoff_set.onoff);
        }
        break;
    case ESP_BLE_MESH_GENERIC_SERVER_RECV_GET_MSG_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_SERVER_RECV_GET_MSG_EVT");
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET) {
            // Only authenticated messages from the gateway primary address
            // refresh connectivity. A provisioned node without reachable gateway blinks.
            if (param->ctx.addr == 0x0001) automesh_led_mark_alive();
            srv = (esp_ble_mesh_gen_onoff_srv_t *)param->model->user_data;
            ESP_LOGI(TAG, "onoff 0x%02x", srv->state.onoff);
            example_handle_gen_onoff_msg(param->model, &param->ctx, NULL);
        }
        break;
    case ESP_BLE_MESH_GENERIC_SERVER_RECV_SET_MSG_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_SERVER_RECV_SET_MSG_EVT");
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
            param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
            ESP_LOGI(TAG, "onoff 0x%02x, tid 0x%02x", param->value.set.onoff.onoff, param->value.set.onoff.tid);
            if (param->value.set.onoff.op_en) {
                ESP_LOGI(TAG, "trans_time 0x%02x, delay 0x%02x",
                    param->value.set.onoff.trans_time, param->value.set.onoff.delay);
            }
            example_handle_gen_onoff_msg(param->model, &param->ctx, &param->value.set.onoff);
        }
        break;
    default:
        ESP_LOGE(TAG, "Unknown Generic Server event 0x%02x", event);
        break;
    }
}

static void example_ble_mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                              esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        switch (param->ctx.recv_op) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD");
            ESP_LOGI(TAG, "net_idx 0x%04x, app_idx 0x%04x",
                param->value.state_change.appkey_add.net_idx,
                param->value.state_change.appkey_add.app_idx);
            ESP_LOG_BUFFER_HEX("AppKey", param->value.state_change.appkey_add.app_key, 16);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND");
            ESP_LOGI(TAG, "elem_addr 0x%04x, app_idx 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_app_bind.element_addr,
                param->value.state_change.mod_app_bind.app_idx,
                param->value.state_change.mod_app_bind.company_id,
                param->value.state_change.mod_app_bind.model_id);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD");
            ESP_LOGI(TAG, "elem_addr 0x%04x, sub_addr 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_sub_add.element_addr,
                param->value.state_change.mod_sub_add.sub_addr,
                param->value.state_change.mod_sub_add.company_id,
                param->value.state_change.mod_sub_add.model_id);
            break;
        default:
            break;
        }
    }
}

static void automesh_locate_cb(esp_ble_mesh_model_cb_event_t event,
                               esp_ble_mesh_model_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT ||
        param->model_operation.model != &locate_models[0] ||
        !param->model_operation.ctx) return;

    const esp_ble_mesh_msg_ctx_t *ctx = param->model_operation.ctx;
    if (param->model_operation.opcode == AUTOMESH_PEER_HELLO) {
        if (param->model_operation.length == 2 &&
            param->model_operation.msg && esp_ble_mesh_node_is_provisioned())
            automesh_note_peer(ctx->addr);
        return;
    }
    if (automesh_node_ota_command(&locate_models[0],ctx,
                                 param->model_operation.opcode,
                                 param->model_operation.msg,
                                 param->model_operation.length)) return;
    // Only the approved gateway may query firmware or trigger locating.
    // The BLE Mesh stack checks the device's AppKey before dispatch.
    if (ctx->addr != 0x0001 ||
        !ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst)) return;
    if (param->model_operation.opcode == AUTOMESH_FW_VERSION_GET) {
        if (param->model_operation.length != 0) return;
        const esp_app_desc_t *app = esp_app_get_description();
        char version[32];
        // Hash prefix identifies this exact ELF build, not a Git commit SHA.
        int count = snprintf(version, sizeof(version), "%.*s %s OTA1 %02x%02x%02x%02x",
                             12, app->version,
#if CONFIG_IDF_TARGET_ESP32C3
                             "C3",
#else
                             "S3",
#endif
                             app->app_elf_sha256[0],
                             app->app_elf_sha256[1], app->app_elf_sha256[2],
                             app->app_elf_sha256[3]);
        if (count <= 0 || count >= (int)sizeof(version)) return;
        esp_err_t result = esp_ble_mesh_server_model_send_msg(
            &locate_models[0], param->model_operation.ctx,
            AUTOMESH_FW_VERSION_STATUS, (uint16_t)count, (uint8_t *)version);
        if (result != ESP_OK)
            ESP_LOGW(TAG, "Firmware version reply failed: %s", esp_err_to_name(result));
        return;
    }
    if (param->model_operation.opcode != AUTOMESH_LOCATE_REQ ||
        param->model_operation.length != 3 ||
        !param->model_operation.msg) return;
    const uint8_t *payload=param->model_operation.msg;
    uint8_t seconds=payload[2];
    if (seconds < 1 || seconds > 30)return;
    automesh_led_mark_alive();
    automesh_led_locate(seconds);
    // Echo the request id + duration as acknowledgment to the gateway.
    esp_err_t e=esp_ble_mesh_server_model_send_msg(
        &locate_models[0],param->model_operation.ctx,
        AUTOMESH_LOCATE_STATUS,3,(uint8_t*)payload);
    if (e!=ESP_OK)ESP_LOGW(TAG,"Locate status reply error: %s",esp_err_to_name(e));
}

static esp_err_t ble_mesh_init(void)
{
    esp_err_t err = ESP_OK;

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_config_server_callback(example_ble_mesh_config_server_cb);
    esp_ble_mesh_register_generic_server_callback(example_ble_mesh_generic_server_cb);
    esp_ble_mesh_register_custom_model_callback(automesh_locate_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mesh stack (err %d)", err);
        return err;
    }

    // Never erase Flash in the BLE host callback: start a dedicated OTA
    // worker before provisioning activates Mesh receive callbacks.
    err = automesh_node_ota_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mesh OTA worker failed to start: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ble_mesh_node_prov_enable((esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable mesh node (err %d)", err);
        return err;
    }

    ESP_LOGI(TAG, "BLE Mesh Node initialized");

    board_led_operation(LED_G, LED_ON);

    return err;
}

static void automesh_mark_boot_healthy(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (p && esp_ota_get_state_partition(p, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) ESP_LOGE(TAG, "Failed to accept OTA app: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing...");

    board_init();
    automesh_led_start();

    err=nvs_flash_init();
    if(err!=ESP_OK) {
        // Never silently destroy Wi-Fi credentials or Mesh keys on a
        // storage error. An explicit, backed-up recovery is required.
        ESP_LOGE(TAG,"NVS init failed: %s. No automatic erase performed.",
                 esp_err_to_name(err));
        return;
    }

    err = bluetooth_init();
    if (err) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return;
    }

    // Remove only the obsolete legacy static-OOB key.  Preserve all Mesh
    // provisioning data, keys, device address and other NVS namespaces.
    nvs_handle_t legacy_nvs;
    esp_err_t old_oob = nvs_open("automesh", NVS_READWRITE, &legacy_nvs);
    if (old_oob == ESP_OK) {
        old_oob = nvs_erase_key(legacy_nvs, "oob_key");
        if (old_oob == ESP_OK) {
            old_oob = nvs_commit(legacy_nvs);
            if (old_oob != ESP_OK)
                ESP_LOGW(TAG, "Legacy OOB cleanup commit failed: %s", esp_err_to_name(old_oob));
        } else if (old_oob != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Legacy OOB cleanup failed: %s", esp_err_to_name(old_oob));
        }
        nvs_close(legacy_nvs);
    }

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    err = ble_mesh_init();
    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
        return;
    }
    automesh_led_set_registered(esp_ble_mesh_node_is_provisioned());
    configASSERT(xTaskCreate(automesh_peer_task,"mesh_peers",3072,
                             NULL,3,NULL)==pdPASS);
    automesh_mark_boot_healthy();
}



