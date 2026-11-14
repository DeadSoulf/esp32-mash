#pragma once
#include "esp_ble_mesh_defs.h"
#define AUTOMESH_VENDOR_CID 0x02E5 /* Espressif test example CID, not production allocation */
#define AUTOMESH_LOCATE_SRV_MODEL 0xA201
#define AUTOMESH_LOCATE_CLI_MODEL 0xA202
#define AUTOMESH_LOCATE_REQ ESP_BLE_MESH_MODEL_OP_3(0x32, AUTOMESH_VENDOR_CID)
#define AUTOMESH_LOCATE_STATUS ESP_BLE_MESH_MODEL_OP_3(0x33, AUTOMESH_VENDOR_CID)
/* Request: 2-byte little-endian request id, 1-byte duration (1-30 seconds).
 * Status: 2-byte request id, 1-byte accepted duration. */
/* Read-only AppKey-authorized firmware reporting. GET: empty payload;
 * STATUS: 1..31 ASCII bytes of actual running firmware identity. */
#define AUTOMESH_FW_VERSION_GET ESP_BLE_MESH_MODEL_OP_3(0x34, AUTOMESH_VENDOR_CID)
#define AUTOMESH_FW_VERSION_STATUS ESP_BLE_MESH_MODEL_OP_3(0x35, AUTOMESH_VENDOR_CID)
