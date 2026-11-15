#include "automesh_topology.h"
#include "automesh_bridge.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_ble_mesh_defs.h"

/* Maximum registered nodes × 3 reported neighbors, plus gateway's direct
 * observations. Old reports age out; no NVS storage or history is kept. */
#define AUTOMESH_TOPOLOGY_LINKS 384
#define AUTOMESH_TOPOLOGY_TTL_US 210000000LL

typedef struct {
    uint16_t source;
    uint16_t target;
    int8_t rssi;
    int64_t observed_at;
} topology_link_t;

static topology_link_t links[AUTOMESH_TOPOLOGY_LINKS];
static portMUX_TYPE links_guard=portMUX_INITIALIZER_UNLOCKED;

static void record_locked(uint16_t source, uint16_t target, int8_t rssi,
                          int64_t now)
{
    int index=-1, oldest=0;
    for (int i=0;i<AUTOMESH_TOPOLOGY_LINKS;i++) {
        if (links[i].source==source && links[i].target==target) {
            index=i; break;
        }
        if (!links[i].source && index<0)index=i;
        if (links[i].observed_at<links[oldest].observed_at)oldest=i;
    }
    if(index<0)index=oldest;
    links[index]=(topology_link_t){
        .source=source,.target=target,.rssi=rssi,.observed_at=now
    };
}

void automesh_topology_note_direct(uint16_t neighbor, int8_t rssi)
{
    if(!ESP_BLE_MESH_ADDR_IS_UNICAST(neighbor)||neighbor==0x0001||
       rssi>=0 || rssi==-128)return;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&links_guard);
    record_locked(0x0001,neighbor,rssi,now);
    portEXIT_CRITICAL(&links_guard);
}

void automesh_topology_note_report(uint16_t reporter,
                                   const uint8_t *data, size_t length)
{
    if(!ESP_BLE_MESH_ADDR_IS_UNICAST(reporter)||reporter==0x0001||
       !data || length<1 || data[0]>3 || length!=1u+3u*data[0])
        return;
    // A report is a complete fresh snapshot for this reporter.
    // Do not retain stale edges after it reports zero nearby devices.
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&links_guard);
    for(int i=0;i<AUTOMESH_TOPOLOGY_LINKS;i++)
        if(links[i].source==reporter)
            memset(&links[i],0,sizeof(links[i]));
    for(unsigned j=0;j<data[0];j++) {
        uint16_t neighbor=(uint16_t)data[1+3*j] |
                          ((uint16_t)data[2+3*j]<<8);
        if(!ESP_BLE_MESH_ADDR_IS_UNICAST(neighbor)||neighbor==reporter)
            continue;
        int8_t rssi=(int8_t)data[3+3*j];
        if(rssi>=0 || rssi==-128)continue;
        record_locked(reporter,neighbor,rssi,now);
    }
    portEXIT_CRITICAL(&links_guard);
}

int automesh_topology_json(char *out, size_t cap)
{
    if (!out || cap<80)return -1;
    int offset=snprintf(out,cap,"{\"links\":[");
    if(offset<0 || (size_t)offset>=cap)return -1;
    int64_t now=esp_timer_get_time();
    bool first=true;
    for(int i=0;i<AUTOMESH_TOPOLOGY_LINKS;i++) {
        topology_link_t link;
        portENTER_CRITICAL(&links_guard);
        link=links[i];
        portEXIT_CRITICAL(&links_guard);
        if(!link.source || !link.observed_at ||
           now<link.observed_at ||
           now-link.observed_at>AUTOMESH_TOPOLOGY_TTL_US)
            continue;
        // Only expose links between approved members. Old/deleted/pending
        // nodes must never appear in the graph via stale radio reports.
        if(!automesh_mesh_address_approved(link.source) ||
           !automesh_mesh_address_approved(link.target))
            continue;
        long age=(long)((now-link.observed_at)/1000000LL);
        int n=snprintf(out+offset,cap-(size_t)offset,
                       "%s{\"source\":%u,\"target\":%u,\"rssi\":%d,\"age\":%ld}",
                       first?"":",",(unsigned)link.source,
                       (unsigned)link.target,(int)link.rssi,age);
        if(n<0 || (size_t)n>=cap-(size_t)offset)return -1;
        offset+=n;
        first=false;
    }
    int n=snprintf(out+offset,cap-(size_t)offset,
                   "],\"valid_for_seconds\":210}");
    if(n<0 || (size_t)n>=cap-(size_t)offset)return -1;
    return offset+n;
}
