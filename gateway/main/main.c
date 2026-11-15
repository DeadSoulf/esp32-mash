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
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"

#include "ble_mesh_example_init.h"
#include "automesh_bridge.h"
#include "automesh_status_led.h"
#include "automesh_web.h"
#include "automesh_locate.h"
#include "automesh_ota_protocol.h"
#include "automesh_gateway_node_ota.h"

#define TAG "EXAMPLE"

#define LED_OFF             0x0
#define LED_ON              0x1

#define CID_ESP             0x02E5

#define PROV_OWN_ADDR       0x0001

#define MSG_SEND_TTL        3
#define MSG_TIMEOUT         0
#define MSG_ROLE            ROLE_PROVISIONER

#define COMP_DATA_PAGE_0    0x00

#define APP_KEY_IDX         0x0000
#define APP_KEY_OCTET       0x12

static uint8_t dev_uuid[16];

typedef struct {
    uint8_t  uuid[16];
    uint16_t unicast;
    uint8_t  elem_num;
    uint8_t  onoff;
} esp_ble_mesh_node_info_t;

static esp_ble_mesh_node_info_t nodes[CONFIG_BLE_MESH_MAX_PROV_NODES] = {0};

static void automesh_store_nodes(void)
{
    nvs_handle_t h;
    esp_err_t err=nvs_open("meshdb",NVS_READWRITE,&h);
    if(err!=ESP_OK) {
        ESP_LOGE(TAG,"Failed to open node database: %s",esp_err_to_name(err));
        return;
    }
    err=nvs_set_blob(h,"nodes",nodes,sizeof(nodes));
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    if(err!=ESP_OK)ESP_LOGE(TAG,"Could not persist Mesh nodes: %s",esp_err_to_name(err));
}
void automesh_gateway_forget_node(const uint8_t uuid[16])
{
    if (!uuid) return;
    for (int i=0;i<ARRAY_SIZE(nodes);++i) {
        if (nodes[i].elem_num && memcmp(nodes[i].uuid,uuid,16)==0)
            memset(&nodes[i],0,sizeof(nodes[i]));
    }
    // Also update the legacy mirror to prevent UI-only ghost nodes on restart.
    automesh_store_nodes();
}

static void automesh_restore_nodes(void)
{
    // The ESP-IDF Provisioner node table is authoritative: unlike the old
    // application RAM array, the SDK persists this table with Mesh keys.
    memset(nodes,0,sizeof(nodes));
    int recovered=0;
    const esp_ble_mesh_node_t **table=esp_ble_mesh_provisioner_get_node_table_entry();
    if(table) {
        for(int i=0;i<CONFIG_BLE_MESH_MAX_PROV_NODES;i++) {
            const esp_ble_mesh_node_t *item=table[i];
            if(!item || !ESP_BLE_MESH_ADDR_IS_UNICAST(item->unicast_addr) ||
               item->element_num==0 || recovered>=ARRAY_SIZE(nodes))continue;
            esp_ble_mesh_node_info_t *entry=&nodes[recovered++];
            memcpy(entry->uuid,item->dev_uuid,16);
            entry->unicast=item->unicast_addr;
            entry->elem_num=item->element_num;
            automesh_bridge_restore(item->dev_uuid,item->unicast_addr);
        }
    }
    // A migration fallback only exposes the old registry as offline. It does
    // NOT assume missing SDK Mesh keys are restored and never sends messages
    // to an address without a corresponding native provisioned node.
    if(recovered==0) {
        nvs_handle_t h;
        if(nvs_open("meshdb",NVS_READONLY,&h)==ESP_OK) {
            esp_ble_mesh_node_info_t previous[CONFIG_BLE_MESH_MAX_PROV_NODES]={0};
            size_t len=sizeof(previous);
            if(nvs_get_blob(h,"nodes",previous,&len)==ESP_OK &&
               len==sizeof(previous)) {
                for(int i=0;i<ARRAY_SIZE(previous);i++) {
                    if(previous[i].elem_num && ESP_BLE_MESH_ADDR_IS_UNICAST(previous[i].unicast))
                        automesh_bridge_restore(previous[i].uuid,previous[i].unicast);
                }
            }
            nvs_close(h);
        }
    }
    automesh_bridge_mesh_ready();
    ESP_LOGI(TAG,"Native provisioner database: %d registered nodes",recovered);
}
static struct esp_ble_mesh_key {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t  app_key[16];
} prov_key;

static esp_ble_mesh_client_t config_client;
static esp_ble_mesh_client_t onoff_client;

static const esp_ble_mesh_client_op_pair_t locate_op_pairs[] = {
    {AUTOMESH_LOCATE_REQ,AUTOMESH_LOCATE_STATUS},
    {AUTOMESH_FW_VERSION_GET,AUTOMESH_FW_VERSION_STATUS},
};
static esp_ble_mesh_client_t locate_client = {
    .op_pair_size=ARRAY_SIZE(locate_op_pairs),
    .op_pair=locate_op_pairs,
};
static esp_ble_mesh_model_op_t locate_client_op[] = {
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_LOCATE_STATUS,3),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_FW_VERSION_STATUS,1),
    ESP_BLE_MESH_MODEL_OP(AUTOMESH_NODE_OTA_STATUS,7),
    ESP_BLE_MESH_MODEL_OP_END,
};
static esp_ble_mesh_model_t locate_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(AUTOMESH_VENDOR_CID,AUTOMESH_LOCATE_CLI_MODEL,
                              locate_client_op,NULL,&locate_client),
};
static uint16_t locate_seq;
static portMUX_TYPE locate_lock=portMUX_INITIALIZER_UNLOCKED;
typedef enum { LOC_IDLE, LOC_BINDING, LOC_SENT, LOC_CONFIRMED,
               LOC_TIMEOUT, LOC_FAILED } locate_phase_t;
typedef struct {
    uint16_t address;
    uint16_t request_id;
    uint8_t seconds;
    locate_phase_t phase;
    int64_t started_us;
} locate_session_t;
static locate_session_t locate_session={0};
static esp_err_t automesh_send_locate_after_bind(uint16_t address);
static esp_err_t automesh_gateway_request_version(uint16_t address);

static void locate_set_phase(uint16_t addr,locate_phase_t from,locate_phase_t to)
{
    portENTER_CRITICAL(&locate_lock);
    if(locate_session.address==addr && locate_session.phase==from)
        locate_session.phase=to;
    portEXIT_CRITICAL(&locate_lock);
}
static bool locate_is_binding(uint16_t addr)
{
    bool yes;
    portENTER_CRITICAL(&locate_lock);
    yes=locate_session.address==addr && locate_session.phase==LOC_BINDING;
    portEXIT_CRITICAL(&locate_lock);
    return yes;
}
int automesh_gateway_locate_status_json(char *out,size_t length)
{
    if(!out || length<100)return -1;
    locate_session_t current;
    portENTER_CRITICAL(&locate_lock);
    if((locate_session.phase==LOC_BINDING || locate_session.phase==LOC_SENT) &&
       esp_timer_get_time()-locate_session.started_us>18000000LL)
        locate_session.phase=LOC_TIMEOUT;
    current=locate_session;
    portEXIT_CRITICAL(&locate_lock);
    const char *phases[]={"idle","binding","sent","confirmed","timeout","failed"};
    return snprintf(out,length,
      "{\"address\":%u,\"request_id\":%u,\"seconds\":%u,\"status\":\"%s\"}",
      (unsigned)current.address,(unsigned)current.request_id,
      (unsigned)current.seconds,phases[current.phase]);
}



static esp_ble_mesh_cfg_srv_t config_server = {
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
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

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(NULL, &onoff_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, locate_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .prov_uuid           = dev_uuid,
    .prov_unicast_addr   = PROV_OWN_ADDR,
    .prov_start_address  = 0x0005,
    .prov_attention      = 0x00,
    .prov_algorithm      = 0x00,
    .prov_pub_key_oob    = 0x00,
    .prov_static_oob_val = NULL,
    .prov_static_oob_len = 0x00,
    .flags               = 0x00,
    .iv_index            = 0x00,
};

static esp_err_t example_ble_mesh_store_node_info(const uint8_t uuid[16], uint16_t unicast,
                                                  uint8_t elem_num, uint8_t onoff_state)
{
    int i;

    if (!uuid || !ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Judge if the device has been provisioned before */
    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!memcmp(nodes[i].uuid, uuid, 16)) {
            ESP_LOGW(TAG, "%s: reprovisioned device 0x%04x", __func__, unicast);
            nodes[i].unicast = unicast;
            nodes[i].elem_num = elem_num;
            nodes[i].onoff = onoff_state;
            return ESP_OK;
        }
    }

    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (nodes[i].unicast == ESP_BLE_MESH_ADDR_UNASSIGNED) {
            memcpy(nodes[i].uuid, uuid, 16);
            nodes[i].unicast = unicast;
            nodes[i].elem_num = elem_num;
            nodes[i].onoff = onoff_state;
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

static esp_ble_mesh_node_info_t *example_ble_mesh_get_node_info(uint16_t unicast)
{
    int i;

    if (!ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return NULL;
    }

    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (nodes[i].unicast <= unicast &&
                nodes[i].unicast + nodes[i].elem_num > unicast) {
            return &nodes[i];
        }
    }

    return NULL;
}

static esp_err_t example_ble_mesh_set_msg_common(esp_ble_mesh_client_common_param_t *common,
                                                 esp_ble_mesh_node_info_t *node,
                                                 esp_ble_mesh_model_t *model, uint32_t opcode)
{
    if (!common || !node || !model) {
        return ESP_ERR_INVALID_ARG;
    }

    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = prov_key.net_idx;
    common->ctx.app_idx = prov_key.app_idx;
    common->ctx.addr = node->unicast;
    common->ctx.send_ttl = MSG_SEND_TTL;
    common->msg_timeout = MSG_TIMEOUT;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    common->msg_role = MSG_ROLE;
#endif

    return ESP_OK;
}

static esp_err_t prov_complete(int node_idx, const esp_ble_mesh_octet16_t uuid,
                               uint16_t unicast, uint8_t elem_num, uint16_t net_idx)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_get_state_t get_state = {0};
    esp_ble_mesh_node_info_t *node = NULL;
    char name[11] = {0};
    int err;

    ESP_LOGI(TAG, "node index: 0x%x, unicast address: 0x%02x, element num: %d, netkey index: 0x%02x",
             node_idx, unicast, elem_num, net_idx);
    ESP_LOGI(TAG, "device uuid: %s", bt_hex(uuid, 16));

    sprintf(name, "%s%d", "NODE-", node_idx);
    err = esp_ble_mesh_provisioner_set_node_name(node_idx, name);
    if (err) {
        ESP_LOGE(TAG, "%s: Set node name failed", __func__);
        return ESP_FAIL;
    }

    err = example_ble_mesh_store_node_info(uuid, unicast, elem_num, LED_OFF);
    if (err) {
        ESP_LOGE(TAG, "%s: Store node info failed", __func__);
        return ESP_FAIL;
    }
    automesh_store_nodes();

    node = example_ble_mesh_get_node_info(unicast);
    if (!node) {
        ESP_LOGE(TAG, "%s: Get node info failed", __func__);
        return ESP_FAIL;
    }

    example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    get_state.comp_data_get.page = COMP_DATA_PAGE_0;
    err = esp_ble_mesh_config_client_get_state(&common, &get_state);
    if (err) {
        ESP_LOGE(TAG, "%s: Send config comp data get failed", __func__);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void prov_link_open(esp_ble_mesh_prov_bearer_t bearer)
{
    ESP_LOGI(TAG, "%s link open", bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
}

static void prov_link_close(esp_ble_mesh_prov_bearer_t bearer, uint8_t reason)
{
    ESP_LOGI(TAG, "%s link close, reason 0x%02x",
             bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT", reason);
    automesh_provisioning_closed();
}

static void recv_unprov_adv_pkt(uint8_t dev_uuid[16], uint8_t addr[BD_ADDR_LEN],
                                esp_ble_mesh_addr_type_t addr_type, uint16_t oob_info,
                                uint8_t adv_type, esp_ble_mesh_prov_bearer_t bearer)
{
    // Discovery NEVER queues a device for provisioning.
    // Provisioning starts only after an explicit command from the web gateway.
    (void)adv_type;
    automesh_candidate(dev_uuid, addr, addr_type, oob_info, bearer);
}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT, err_code %d", param->provisioner_prov_enable_comp.err_code);
        if(param->provisioner_prov_enable_comp.err_code==ESP_OK)
            automesh_restore_nodes();
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT, err_code %d", param->provisioner_prov_disable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT");
        recv_unprov_adv_pkt(param->provisioner_recv_unprov_adv_pkt.dev_uuid, param->provisioner_recv_unprov_adv_pkt.addr,
                            param->provisioner_recv_unprov_adv_pkt.addr_type, param->provisioner_recv_unprov_adv_pkt.oob_info,
                            param->provisioner_recv_unprov_adv_pkt.adv_type, param->provisioner_recv_unprov_adv_pkt.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        prov_link_open(param->provisioner_prov_link_open.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        prov_link_close(param->provisioner_prov_link_close.bearer, param->provisioner_prov_link_close.reason);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        prov_complete(param->provisioner_prov_complete.node_idx, param->provisioner_prov_complete.device_uuid,
                      param->provisioner_prov_complete.unicast_addr, param->provisioner_prov_complete.element_num,
                      param->provisioner_prov_complete.netkey_idx);
        automesh_provisioned(param->provisioner_prov_complete.device_uuid,
                             param->provisioner_prov_complete.unicast_addr);
        break;
    case ESP_BLE_MESH_PROVISIONER_DELETE_NODE_WITH_UUID_COMP_EVT: {
        const uint8_t *uuid=param->provisioner_delete_node_with_uuid_comp.uuid;
        esp_err_t result=param->provisioner_delete_node_with_uuid_comp.err_code;
        if(result==ESP_OK)
            automesh_gateway_forget_node(uuid);
        else
            ESP_LOGE(TAG,"Mesh node deletion failed: %s",esp_err_to_name(result));
        automesh_remove_completed(uuid,result);
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT, err_code %d", param->provisioner_add_unprov_dev_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT, err_code %d", param->provisioner_set_dev_uuid_match_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT: {
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT, err_code %d", param->provisioner_set_node_name_comp.err_code);
        if (param->provisioner_set_node_name_comp.err_code == ESP_OK) {
            const char *name = NULL;
            name = esp_ble_mesh_provisioner_get_node_name(param->provisioner_set_node_name_comp.node_index);
            if (!name) {
                ESP_LOGE(TAG, "Get node name failed");
                return;
            }
            ESP_LOGI(TAG, "Node %d name is: %s", param->provisioner_set_node_name_comp.node_index, name);
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT: {
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT, err_code %d", param->provisioner_add_app_key_comp.err_code);
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            esp_err_t err = 0;
            prov_key.app_idx = param->provisioner_add_app_key_comp.app_idx;
            err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(PROV_OWN_ADDR, prov_key.app_idx,
                    ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, ESP_BLE_MESH_CID_NVAL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Provisioner bind local model appkey failed");
                return;
            }
            err=esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                PROV_OWN_ADDR, prov_key.app_idx,
                AUTOMESH_LOCATE_CLI_MODEL, AUTOMESH_VENDOR_CID);
            if(err!=ESP_OK) {
                ESP_LOGE(TAG,"Provisioner bind Locate client appkey failed: %s",esp_err_to_name(err));
                return;
            }
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT, err_code %d", param->provisioner_bind_app_key_to_model_comp.err_code);
        break;
    default:
        break;
    }

    return;
}

static void example_ble_mesh_config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                              esp_ble_mesh_cfg_client_cb_param_t *param)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_node_info_t *node = NULL;
    uint32_t opcode;
    uint16_t addr;
    int err;

    opcode = param->params->opcode;
    addr = param->params->ctx.addr;

    ESP_LOGI(TAG, "%s, error_code = 0x%02x, event = 0x%02x, addr: 0x%04x, opcode: 0x%04" PRIx32,
             __func__, param->error_code, event, param->params->ctx.addr, opcode);

    if (param->error_code) {
        if(opcode==ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND)
            locate_set_phase(addr,LOC_BINDING,LOC_FAILED);
        ESP_LOGE(TAG, "Send config client message failed, opcode 0x%04" PRIx32, opcode);
        return;
    }

    node = example_ble_mesh_get_node_info(addr);
    if (!node) {
        ESP_LOGE(TAG, "%s: Get node info failed", __func__);
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET: {
            ESP_LOGI(TAG, "composition data %s", bt_hex(param->status_cb.comp_data_status.composition_data->data,
                     param->status_cb.comp_data_status.composition_data->len));
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set_state.app_key_add.net_idx = prov_key.net_idx;
            set_state.app_key_add.app_idx = prov_key.app_idx;
            memcpy(set_state.app_key_add.app_key, prov_key.app_key, 16);
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config AppKey Add failed", __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD: {
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
            set_state.model_app_bind.element_addr = node->unicast;
            set_state.model_app_bind.model_app_idx = prov_key.app_idx;
            set_state.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV;
            set_state.model_app_bind.company_id = ESP_BLE_MESH_CID_NVAL;
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Model App Bind failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND: {
            if(param->status_cb.model_app_status.company_id==AUTOMESH_VENDOR_CID &&
               param->status_cb.model_app_status.model_id==AUTOMESH_LOCATE_SRV_MODEL) {
                if(param->status_cb.model_app_status.status!=0) {
                    locate_set_phase(addr,LOC_BINDING,LOC_FAILED);
                    ESP_LOGW(TAG,"Vendor Locate model rejected AppKey binding (status %u)",
                             (unsigned)param->status_cb.model_app_status.status);
                } else {
                    if(locate_is_binding(addr)) automesh_send_locate_after_bind(addr);
                    (void)automesh_gateway_request_version(addr);
                }
                break;
            }
            if(param->status_cb.model_app_status.status!=0) {
                ESP_LOGW(TAG,"Model AppKey bind failed (status %u)",
                         (unsigned)param->status_cb.model_app_status.status);
                break;
            }
            if(param->status_cb.model_app_status.company_id==ESP_BLE_MESH_CID_NVAL &&
               param->status_cb.model_app_status.model_id==ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV) {
                // Bind the new node vendor Locate model with the same approved AppKey.
                esp_ble_mesh_cfg_client_set_state_t bind={0};
                example_ble_mesh_set_msg_common(&common,node,config_client.model,
                                                 ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
                bind.model_app_bind.element_addr=node->unicast;
                bind.model_app_bind.model_app_idx=prov_key.app_idx;
                bind.model_app_bind.model_id=AUTOMESH_LOCATE_SRV_MODEL;
                bind.model_app_bind.company_id=AUTOMESH_VENDOR_CID;
                err=esp_ble_mesh_config_client_set_state(&common,&bind);
                if(err!=ESP_OK)ESP_LOGE(TAG,"Vendor locate bind failed: %s",esp_err_to_name(err));
                break;
            }
            esp_ble_mesh_generic_client_get_state_t get_state = {0};
            example_ble_mesh_set_msg_common(&common, node, onoff_client.model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET);
            err = esp_ble_mesh_generic_client_get_state(&common, &get_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Generic OnOff Get failed", __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_PUBLISH_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS:
            ESP_LOG_BUFFER_HEX("composition data %s", param->status_cb.comp_data_status.composition_data->data,
                               param->status_cb.comp_data_status.composition_data->len);
            break;
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_STATUS:
            break;
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
        if(opcode==ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND &&
           locate_is_binding(addr)) {
            locate_set_phase(addr,LOC_BINDING,LOC_TIMEOUT);
            ESP_LOGW(TAG,"Locate binding timeout for 0x%04x",addr);
            break;
        }
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET: {
            esp_ble_mesh_cfg_client_get_state_t get_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
            get_state.comp_data_get.page = COMP_DATA_PAGE_0;
            err = esp_ble_mesh_config_client_get_state(&common, &get_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Composition Data Get failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD: {
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set_state.app_key_add.net_idx = prov_key.net_idx;
            set_state.app_key_add.app_idx = prov_key.app_idx;
            memcpy(set_state.app_key_add.app_key, prov_key.app_key, 16);
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config AppKey Add failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND: {
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
            set_state.model_app_bind.element_addr = node->unicast;
            set_state.model_app_bind.model_app_idx = prov_key.app_idx;
            set_state.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV;
            set_state.model_app_bind.company_id = ESP_BLE_MESH_CID_NVAL;
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Model App Bind failed", __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    default:
        ESP_LOGE(TAG, "Not a config client status message event");
        break;
    }
}

static void example_ble_mesh_generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                                               esp_ble_mesh_generic_client_cb_param_t *param)
{
    esp_ble_mesh_node_info_t *node = NULL;
    uint32_t opcode;
    uint16_t addr;

    opcode = param->params->opcode;
    addr = param->params->ctx.addr;

    ESP_LOGI(TAG, "%s, error_code = 0x%02x, event = 0x%02x, addr: 0x%04x, opcode: 0x%04" PRIx32,
             __func__, param->error_code, event, param->params->ctx.addr, opcode);

    if (param->error_code) {
        ESP_LOGE(TAG, "Send generic client message failed, opcode 0x%04" PRIx32, opcode);
        return;
    }

    node = example_ble_mesh_get_node_info(addr);
    if (!node) {
        ESP_LOGE(TAG, "%s: Get node info failed", __func__);
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET: {
            // A status response proves the node is reachable at this moment.
            automesh_note_reply(addr);
            node->onoff = param->status_cb.onoff_status.present_onoff;
            break;
        }
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET:
            node->onoff = param->status_cb.onoff_status.present_onoff;
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET onoff: 0x%02x", node->onoff);
            break;
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
        // No immediate retry: health task will poll again after 10 seconds.
        ESP_LOGW(TAG, "Mesh node 0x%04x did not answer; will retry later", addr);
        break;
    default:
        ESP_LOGE(TAG, "Not a generic client status message event");
        break;
    }
}

static esp_err_t automesh_gateway_request_version(uint16_t address)
{
    if (!locate_client.model || !ESP_BLE_MESH_ADDR_IS_UNICAST(address) ||
        !esp_ble_mesh_provisioner_get_node_with_addr(address))
        return ESP_ERR_NOT_FOUND;
    esp_ble_mesh_msg_ctx_t ctx = {0};
    ctx.net_idx = prov_key.net_idx;
    ctx.app_idx = prov_key.app_idx;
    ctx.addr = address;
    ctx.send_ttl = 7;
    return esp_ble_mesh_client_model_send_msg(locate_client.model, &ctx,
        AUTOMESH_FW_VERSION_GET, 0, NULL, 3500, true, ROLE_PROVISIONER);
}

esp_err_t automesh_gateway_ping(uint16_t address)
{
    esp_ble_mesh_node_info_t *node=example_ble_mesh_get_node_info(address);
    if(!node) return ESP_ERR_NOT_FOUND;
    esp_ble_mesh_client_common_param_t common={0};
    esp_ble_mesh_generic_client_get_state_t get_state={0};
    esp_err_t err=example_ble_mesh_set_msg_common(&common,node,onoff_client.model,
                       ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET);
    if(err!=ESP_OK) return err;
    common.msg_timeout=3000;
    return esp_ble_mesh_generic_client_get_state(&common,&get_state);
}

static void automesh_health_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(20000));
    uint32_t firmware_poll_cycle = 0;
    while (true) {
        // Mesh OTA needs airtime for segmented transfer and acknowledgements.
        // Defer health pings/version scans while any node is updating.
        if (automesh_gateway_node_ota_active()) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        for (int i=0; i<ARRAY_SIZE(nodes); ++i) {
            if (nodes[i].elem_num == 0 ||
                !ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) continue;
            esp_ble_mesh_client_common_param_t common={0};
            esp_ble_mesh_generic_client_get_state_t get_state={0};
            if (example_ble_mesh_set_msg_common(&common, &nodes[i],
                  onoff_client.model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET)==ESP_OK) {
                common.msg_timeout=3000;
                esp_err_t e=esp_ble_mesh_generic_client_get_state(&common,&get_state);
                if (e!=ESP_OK) ESP_LOGW(TAG,"Node 0x%04x ping failed: %s",
                                         nodes[i].unicast,esp_err_to_name(e));
            }
            // Status query at boot, then approximately every 60 seconds.
            // This cannot register an unknown device or start an update.
            if ((firmware_poll_cycle % 6u) == 0u)
                (void)automesh_gateway_request_version(nodes[i].unicast);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ++firmware_poll_cycle;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static esp_err_t automesh_send_locate_after_bind(uint16_t address)
{
    if(!locate_is_binding(address))return ESP_ERR_INVALID_STATE;
    locate_session_t item;
    portENTER_CRITICAL(&locate_lock);
    item=locate_session;
    portEXIT_CRITICAL(&locate_lock);
    esp_ble_mesh_msg_ctx_t ctx={0};
    ctx.net_idx=prov_key.net_idx;
    ctx.app_idx=prov_key.app_idx;
    ctx.addr=address;
    ctx.send_ttl=7;
    uint8_t payload[3]={(uint8_t)(item.request_id&0xff),
                        (uint8_t)(item.request_id>>8),item.seconds};
    esp_err_t err=esp_ble_mesh_client_model_send_msg(
        locate_client.model,&ctx,AUTOMESH_LOCATE_REQ,
        sizeof(payload),payload,4000,true,ROLE_PROVISIONER);
    locate_set_phase(address,LOC_BINDING,err==ESP_OK?LOC_SENT:LOC_FAILED);
    if(err!=ESP_OK)ESP_LOGE(TAG,"Locate send failed: %s",esp_err_to_name(err));
    return err;
}
esp_err_t automesh_gateway_locate(uint16_t address,uint8_t duration)
{
    if(!ESP_BLE_MESH_ADDR_IS_UNICAST(address) || duration<1 || duration>30)
        return ESP_ERR_INVALID_ARG;
    // A saved UI registry entry is not proof that the SDK has restored
    // encryption keys; refuse to send until the native table knows the node.
    if(!locate_client.model || !example_ble_mesh_get_node_info(address) ||
       !esp_ble_mesh_provisioner_get_node_with_addr(address))
        return ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&locate_lock);
    int64_t now=esp_timer_get_time();
    if((locate_session.phase==LOC_BINDING || locate_session.phase==LOC_SENT) &&
       now-locate_session.started_us<18000000LL) {
        portEXIT_CRITICAL(&locate_lock);
        return ESP_ERR_INVALID_STATE;
    }
    locate_session=(locate_session_t){
        .address=address,.request_id=++locate_seq,.seconds=duration,
        .phase=LOC_BINDING,.started_us=now
    };
    portEXIT_CRITICAL(&locate_lock);
    // Bind the vendor model again even for nodes provisioned with older
    // firmware. This is idempotent and addresses older unconfigured nodes.
    esp_ble_mesh_node_info_t *node=example_ble_mesh_get_node_info(address);
    esp_ble_mesh_client_common_param_t common={0};
    esp_ble_mesh_cfg_client_set_state_t bind={0};
    example_ble_mesh_set_msg_common(&common,node,config_client.model,
                                     ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
    common.msg_timeout=5000;
    bind.model_app_bind.element_addr=node->unicast;
    bind.model_app_bind.model_app_idx=prov_key.app_idx;
    bind.model_app_bind.model_id=AUTOMESH_LOCATE_SRV_MODEL;
    bind.model_app_bind.company_id=AUTOMESH_VENDOR_CID;
    esp_err_t err=esp_ble_mesh_config_client_set_state(&common,&bind);
    if(err!=ESP_OK) {
        locate_set_phase(address,LOC_BINDING,LOC_FAILED);
        ESP_LOGW(TAG,"Locate model bind rejected: %s",esp_err_to_name(err));
    }
    return err;
}
static void automesh_locate_client_cb(esp_ble_mesh_model_cb_event_t event,
                                     esp_ble_mesh_model_cb_param_t *param)
{
    if(event==ESP_BLE_MESH_MODEL_OPERATION_EVT &&
       param->model_operation.model==&locate_models[0] &&
       param->model_operation.opcode==AUTOMESH_NODE_OTA_STATUS &&
       param->model_operation.ctx && param->model_operation.msg) {
        automesh_gateway_node_ota_on_status(param->model_operation.ctx->addr,
                 param->model_operation.msg,param->model_operation.length);
        return;
    }
    if(event==ESP_BLE_MESH_MODEL_OPERATION_EVT &&
       param->model_operation.model==&locate_models[0] &&
       param->model_operation.opcode==AUTOMESH_FW_VERSION_STATUS &&
       param->model_operation.ctx && param->model_operation.msg &&
       param->model_operation.length > 0 &&
       param->model_operation.length < 32) {
        automesh_note_version(param->model_operation.ctx->addr,
                              param->model_operation.msg,
                              param->model_operation.length);
        return;
    }
    if(event==ESP_BLE_MESH_MODEL_OPERATION_EVT &&
       param->model_operation.model==&locate_models[0] &&
       param->model_operation.opcode==AUTOMESH_LOCATE_STATUS &&
       param->model_operation.length==3 && param->model_operation.msg &&
       param->model_operation.ctx) {
        const uint8_t *msg=param->model_operation.msg;
        const uint16_t id=(uint16_t)msg[0] | ((uint16_t)msg[1]<<8);
        const uint16_t address=param->model_operation.ctx->addr;
        bool confirmed=false;
        portENTER_CRITICAL(&locate_lock);
        if(locate_session.address==address && locate_session.request_id==id &&
           locate_session.phase==LOC_SENT) {
            locate_session.phase=LOC_CONFIRMED;
            confirmed=true;
        }
        portEXIT_CRITICAL(&locate_lock);
        if(confirmed) {
            ESP_LOGI(TAG,"Locate ACK from 0x%04x, request=%u",address,(unsigned)id);
            automesh_note_reply(address);
        }
    } else if (event==ESP_BLE_MESH_CLIENT_MODEL_SEND_TIMEOUT_EVT &&
               param->client_send_timeout.opcode==AUTOMESH_LOCATE_REQ &&
               param->client_send_timeout.ctx) {
        const uint16_t address=param->client_send_timeout.ctx->addr;
        locate_set_phase(address,LOC_SENT,LOC_TIMEOUT);
        ESP_LOGW(TAG,"Locate node 0x%04x did not acknowledge",address);
    } else if(event==ESP_BLE_MESH_MODEL_SEND_COMP_EVT &&
              param->model_send_comp.opcode==AUTOMESH_LOCATE_REQ &&
              param->model_send_comp.err_code) {
        locate_set_phase(locate_session.address,LOC_SENT,LOC_FAILED);
    }
}

static esp_err_t ble_mesh_init(void)
{
    uint8_t match[2] = {0xdd, 0xdd};
    esp_err_t err = ESP_OK;

    prov_key.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    prov_key.app_idx = APP_KEY_IDX;
    ESP_ERROR_CHECK(automesh_load_appkey(prov_key.app_key));

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_config_client_callback(example_ble_mesh_config_client_cb);
    esp_ble_mesh_register_generic_client_callback(example_ble_mesh_generic_client_cb);
    esp_ble_mesh_register_custom_model_callback(automesh_locate_client_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mesh stack (err %d)", err);
        return err;
    }

    // Vendor clients must be initialized explicitly before sending custom messages.
    err=esp_ble_mesh_client_model_init(&locate_models[0]);
    if(err!=ESP_OK) {
        ESP_LOGE(TAG,"Locate vendor client init failed: %s",esp_err_to_name(err));
        return err;
    }
    automesh_gateway_node_ota_init(locate_client.model,prov_key.net_idx,
                                    prov_key.app_idx);
    err = esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0x0, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set matching device uuid (err %d)", err);
        return err;
    }

    err = esp_ble_mesh_provisioner_prov_enable((esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable mesh provisioner (err %d)", err);
        return err;
    }

    err = esp_ble_mesh_provisioner_add_local_app_key(prov_key.app_key, prov_key.net_idx, prov_key.app_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add local AppKey (err %d)", err);
        return err;
    }

    ESP_LOGI(TAG, "BLE Mesh Provisioner initialized");

    return err;
}

void automesh_mark_boot_healthy(void)
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

    err=nvs_flash_init();
    if(err!=ESP_OK) {
        // Never silently destroy Wi-Fi credentials or Mesh keys on a
        // storage error. An explicit, backed-up recovery is required.
        ESP_LOGE(TAG,"NVS init failed: %s. No automatic erase performed.",
                 esp_err_to_name(err));
        return;
    }

    if (!automesh_web_has_network()) {
        // A reliable local setup AP cannot coexist with our continuous
        // Mesh provisioning scanner on C3. Use AP ONLY for Wi-Fi setup.
        ESP_LOGW(TAG, "Setup AP mode; Bluetooth Mesh paused until router Wi-Fi is configured");
        automesh_bridge_init();
        automesh_led_start();
        ESP_ERROR_CHECK(automesh_web_start());
        automesh_mark_boot_healthy();
        return;
    }

    err = bluetooth_init();
    if (err) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return;
    }

    automesh_bridge_init();
    automesh_led_start();

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    err = ble_mesh_init();
    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
        return;
    }
    automesh_restore_nodes();
    automesh_bridge_start();
    ESP_ERROR_CHECK(automesh_web_start());
    // The web Wi-Fi callback confirms pending OTA only after DHCP succeeds.
    configASSERT(xTaskCreate(automesh_health_task, "mesh_health", 4096,
                             NULL, 4, NULL)==pdPASS);
}



