#pragma once
#include <stddef.h>
#include <stdint.h>

/* RAM-only, TTL=1 radio-neighbor observations, NOT fixed Mesh routes.
 * Nodes report their strongest direct neighbors; gateway also records
 * direct neighbor beacons it hears itself.
 */
void automesh_topology_note_direct(uint16_t neighbor, int8_t rssi);
void automesh_topology_note_report(uint16_t reporter, const uint8_t *payload, size_t length);
int automesh_topology_json(char *out, size_t capacity);
