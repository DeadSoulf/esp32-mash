#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_ble_mesh_defs.h"
#include "esp_err.h"
/* Returns true when a command belongs to the node OTA protocol. */
bool automesh_node_ota_command(esp_ble_mesh_model_t *model,
                              const esp_ble_mesh_msg_ctx_t *ctx,
                              uint32_t opcode, const uint8_t *payload,
                              size_t length);
