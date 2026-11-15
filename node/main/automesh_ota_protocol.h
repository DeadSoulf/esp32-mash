#pragma once
/* AutoMesh experimental BLE Mesh OTA transport, AppKey-protected vendor opcodes.
 * The node is updated only after a complete SHA-256 and image validation.
 * This protocol requires both gateway and target node firmware >= 0.3.11-dev.
 */
#include <stdint.h>
#include "esp_ble_mesh_defs.h"
#define AUTOMESH_NODE_OTA_START ESP_BLE_MESH_MODEL_OP_3(0x36, AUTOMESH_VENDOR_CID)
#define AUTOMESH_NODE_OTA_DATA ESP_BLE_MESH_MODEL_OP_3(0x37, AUTOMESH_VENDOR_CID)
#define AUTOMESH_NODE_OTA_FINISH ESP_BLE_MESH_MODEL_OP_3(0x38, AUTOMESH_VENDOR_CID)
#define AUTOMESH_NODE_OTA_ABORT ESP_BLE_MESH_MODEL_OP_3(0x39, AUTOMESH_VENDOR_CID)
#define AUTOMESH_NODE_OTA_STATUS ESP_BLE_MESH_MODEL_OP_3(0x3A, AUTOMESH_VENDOR_CID)
#define AUTOMESH_PEER_HELLO ESP_BLE_MESH_MODEL_OP_3(0x3B, AUTOMESH_VENDOR_CID)
/* OTA1 nodes accept at most 96 bytes. OTA2 nodes accept 224 bytes in one
 * application message, still below the BLE Mesh 384-byte upper-transport SDU.
 * Never send OTA2 packets until the node advertises OTA2 explicitly.
 */
#define AUTOMESH_NODE_OTA_LEGACY_BLOCK 96u
#define AUTOMESH_NODE_OTA_BLOCK 224u
#define AUTOMESH_NODE_OTA_MAX_IMAGE 0x1c0000u
/* START: session_id:2, size:4, chip:'C' or 'S', sha256:32.
 * DATA: session_id:2, offset:4, binary:1..96 (OTA1) or 1..224 (OTA2).
 * FINISH/ABORT: session_id:2.
 * STATUS: session_id:2, next_offset:4, code:1 (0=OK, 1=busy,
 * 2=invalid input, 3=integrity/image failure, 4=flash failure).
 * PEER_HELLO: sequence:2, no response; all-nodes application broadcast.
 */
static inline uint16_t automesh_u16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static inline uint32_t automesh_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void automesh_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void automesh_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
