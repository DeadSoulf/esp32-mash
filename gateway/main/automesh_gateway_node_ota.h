#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ble_mesh_defs.h"

void automesh_gateway_node_ota_init(esp_ble_mesh_model_t *model,
                                    uint16_t net_idx, uint16_t app_idx);
void automesh_gateway_node_ota_on_status(uint16_t source,
                                         const uint8_t *payload, size_t len);
esp_err_t automesh_gateway_node_ota_start(uint16_t address, uint8_t chip,
                                          uint32_t image_size, const uint8_t sha[32],
                                          uint16_t *session);
esp_err_t automesh_gateway_node_ota_chunk(uint32_t offset, const uint8_t *data,
                                          size_t len, uint32_t *confirmed);
esp_err_t automesh_gateway_node_ota_finish(void);
esp_err_t automesh_gateway_node_ota_abort(void);
bool automesh_gateway_node_ota_active(void);
uint32_t automesh_gateway_node_ota_progress(void);
uint16_t automesh_gateway_node_ota_max_chunk(void);
