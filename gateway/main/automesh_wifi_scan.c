#include "automesh_wifi_scan.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#define MAX_WIFI_NETWORKS 24
static const char *TAG = "AUTOMESH_SCAN";
static SemaphoreHandle_t lock;
static bool busy;
static bool scanned;
static uint16_t total;
static wifi_ap_record_t networks[MAX_WIFI_NETWORKS];
static esp_err_t last_error;

esp_err_t automesh_wifi_scan_init(void)
{
    if (!lock) lock=xSemaphoreCreateMutex();
    return lock?ESP_OK:ESP_ERR_NO_MEM;
}
static void scan_task(void *unused)
{
    (void)unused;
    wifi_scan_config_t conf={0};
    conf.scan_type=WIFI_SCAN_TYPE_ACTIVE;
    conf.show_hidden=false;
    conf.scan_time.active.min=0;
    conf.scan_time.active.max=65;
    esp_err_t e=esp_wifi_scan_start(&conf,true);
    wifi_ap_record_t found[MAX_WIFI_NETWORKS]={0};
    uint16_t count=0;
    if(e==ESP_OK) {
        e=esp_wifi_scan_get_ap_num(&count);
        if(e==ESP_OK) {
            if(count>MAX_WIFI_NETWORKS)count=MAX_WIFI_NETWORKS;
            e=esp_wifi_scan_get_ap_records(&count,found);
        } else {
            esp_wifi_clear_ap_list();
        }
    }
    if(e!=ESP_OK)esp_wifi_clear_ap_list();
    if(e==ESP_OK) {
        // Strongest signal first; no background scanning after completion.
        for(unsigned i=0;i<count;i++)
            for(unsigned j=i+1;j<count;j++)
                if(found[j].rssi>found[i].rssi) {
                    wifi_ap_record_t temp=found[i];
                    found[i]=found[j];
                    found[j]=temp;
                }
    }
    if(xSemaphoreTake(lock,portMAX_DELAY)==pdTRUE) {
        last_error=e;
        total=(e==ESP_OK)?count:0;
        if(e==ESP_OK)memcpy(networks,found,sizeof(networks));
        scanned=true;
        busy=false;
        xSemaphoreGive(lock);
    }
    ESP_LOGI(TAG,"Scan finished: %u access points, status %s",
             (unsigned)count,esp_err_to_name(e));
    vTaskDelete(NULL);
}
esp_err_t automesh_wifi_scan_begin(void)
{
    if(!lock)return ESP_ERR_INVALID_STATE;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(300))!=pdTRUE)return ESP_ERR_TIMEOUT;
    if(busy) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    busy=true;
    scanned=false;
    total=0;
    last_error=ESP_OK;
    xSemaphoreGive(lock);
    if(xTaskCreate(scan_task,"wifi_scan",4096,NULL,4,NULL)!=pdPASS) {
        if(xSemaphoreTake(lock,portMAX_DELAY)==pdTRUE) {
            busy=false;
            last_error=ESP_ERR_NO_MEM;
            xSemaphoreGive(lock);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
char *automesh_wifi_scan_json(void)
{
    if(!lock)return NULL;
    wifi_ap_record_t copy[MAX_WIFI_NETWORKS];
    uint16_t count;
    bool active,done;
    esp_err_t err;
    if(xSemaphoreTake(lock,pdMS_TO_TICKS(400))!=pdTRUE)return NULL;
    count=total;
    active=busy;
    done=scanned;
    err=last_error;
    if(count>MAX_WIFI_NETWORKS)count=MAX_WIFI_NETWORKS;
    memcpy(copy,networks,sizeof(copy));
    xSemaphoreGive(lock);
    cJSON *root=cJSON_CreateObject();
    if(!root)return NULL;
    cJSON_AddBoolToObject(root,"scanning",active);
    cJSON_AddBoolToObject(root,"finished",done);
    cJSON_AddStringToObject(root,"error",err==ESP_OK?"":esp_err_to_name(err));
    cJSON *list=cJSON_AddArrayToObject(root,"networks");
    if(!list) {cJSON_Delete(root);return NULL;}
    for(unsigned i=0;i<count;i++) {
        // ESP Wi-Fi records allow 32 SSID bytes with no NUL terminator.
        char ssid[33];
        memcpy(ssid,copy[i].ssid,32);
        ssid[32]='\0';
        if(!ssid[0])continue;
        cJSON *item=cJSON_CreateObject();
        if(!item){cJSON_Delete(root);return NULL;}
        cJSON_AddStringToObject(item,"ssid",ssid);
        cJSON_AddNumberToObject(item,"rssi",copy[i].rssi);
        cJSON_AddNumberToObject(item,"channel",copy[i].primary);
        cJSON_AddBoolToObject(item,"secure",copy[i].authmode!=WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list,item);
    }
    char *answer=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return answer;
}

