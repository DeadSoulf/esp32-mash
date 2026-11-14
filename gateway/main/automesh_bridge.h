#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ble_mesh_provisioning_api.h"

void automesh_bridge_init(void);
void automesh_bridge_mesh_ready(void);
void automesh_bridge_start(void);
void automesh_candidate(const uint8_t uuid[16],const uint8_t addr[6],
                        esp_ble_mesh_addr_type_t type,uint16_t oob_info,
                        esp_ble_mesh_prov_bearer_t bearer);
void automesh_provisioned(const uint8_t uuid[16],uint16_t address);
void automesh_bridge_restore(const uint8_t uuid[16],uint16_t address);
void automesh_provisioning_closed(void);
void automesh_note_reply(uint16_t mesh_address);
esp_err_t automesh_approve_uuid(const char *uuid_hex,const char *oob_hex);
esp_err_t automesh_deny_uuid(const char *uuid_hex);
int automesh_devices_json(char *out,size_t capacity);
esp_err_t automesh_set_name(const char *uuid_hex,const char *name);
esp_err_t automesh_load_appkey(uint8_t key[16]);


