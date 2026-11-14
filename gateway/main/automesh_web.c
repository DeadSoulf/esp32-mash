#include "automesh_web.h"
#include "automesh_wifi_scan.h"
#include "automesh_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "cJSON.h"

#define ADMIN_LEN 32
#define AP_PASSWORD_LEN 16
#define MAX_BODY 256
static const char *TAG="AUTOMESH_WEB";
static char admin_key[ADMIN_LEN+1];
static char ap_password[AP_PASSWORD_LEN+1];
static char ap_ssid[33];
static char sta_ssid[33];
static char sta_pass[64];
static bool setup_mode=true;
static volatile bool using_pending=false;
static volatile bool got_ip=false;
typedef struct {
    char ssid[33];
    char password[64];
} wifi_candidate_t;
static esp_netif_t *wifi_interface=NULL;
static char ip_string[16]="0.0.0.0";
static httpd_handle_t server=NULL;

extern const char gateway_index_html_start[] asm("_binary_gateway_index_html_start");
extern const char gateway_index_html_end[] asm("_binary_gateway_index_html_end");
extern esp_err_t automesh_gateway_ping(uint16_t address);
extern esp_err_t automesh_gateway_locate(uint16_t address,uint8_t seconds);

static esp_err_t secret_get_or_make(nvs_handle_t h,const char *key,char *out,size_t length,bool hex)
{
    size_t sz=length+1;
    esp_err_t e=nvs_get_str(h,key,out,&sz);
    if(e==ESP_OK && strlen(out)==length) return ESP_OK;
    if(e!=ESP_ERR_NVS_NOT_FOUND && e!=ESP_OK) return e;
    static const char alphabet[]="ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<length;i++) {
        out[i]= hex?digits[esp_random()%16]:alphabet[esp_random()%(sizeof(alphabet)-1)];
    }
    out[length]=0;
    e=nvs_set_str(h,key,out);
    if(e==ESP_OK)e=nvs_commit(h);
    return e;
}
static esp_err_t load_credentials(void)
{
    nvs_handle_t h;
    esp_err_t e=nvs_open("meshweb",NVS_READWRITE,&h);
    if(e!=ESP_OK)return e;
    e=secret_get_or_make(h,"ap_pass",ap_password,AP_PASSWORD_LEN,false);
    if(e==ESP_OK)e=secret_get_or_make(h,"admin",admin_key,ADMIN_LEN,true);
    nvs_close(h);
    return e;
}

static bool valid_network(const char *ssid,const char *password)
{
    if(!ssid || !password)return false;
    size_t ss=strlen(ssid),ps=strlen(password);
    return ss>0 && ss<=32 && (ps==0 || (ps>=8 && ps<=63));
}
static bool load_home_network(void)
{
    nvs_handle_t h;
    using_pending=false;
    if(nvs_open("meshweb",NVS_READONLY,&h)!=ESP_OK)return false;
    wifi_candidate_t candidate={0};
    size_t len=sizeof(candidate);
    if(nvs_get_blob(h,"pend_wifi",&candidate,&len)==ESP_OK &&
        len==sizeof(candidate) &&
        candidate.ssid[32]=='\0' && candidate.password[63]=='\0' &&
        valid_network(candidate.ssid,candidate.password)) {
        strlcpy(sta_ssid,candidate.ssid,sizeof(sta_ssid));
        strlcpy(sta_pass,candidate.password,sizeof(sta_pass));
        using_pending=true;
        nvs_close(h);
        return true;
    }
    size_t ss=sizeof(sta_ssid),ps=sizeof(sta_pass);
    esp_err_t a=nvs_get_str(h,"sta_ssid",sta_ssid,&ss);
    esp_err_t b=nvs_get_str(h,"sta_pass",sta_pass,&ps);
    nvs_close(h);
    return a==ESP_OK && b==ESP_OK && valid_network(sta_ssid,sta_pass);
}
bool automesh_web_has_network(void)
{
    return load_home_network();
}
static void promote_pending_wifi(void)
{
    if(!using_pending)return;
    nvs_handle_t h;
    esp_err_t e=nvs_open("meshweb",NVS_READWRITE,&h);
    if(e!=ESP_OK)return;
    e=nvs_set_str(h,"sta_ssid",sta_ssid);
    if(e==ESP_OK)e=nvs_set_str(h,"sta_pass",sta_pass);
    if(e==ESP_OK)e=nvs_erase_key(h,"pend_wifi");
    if(e==ESP_OK)e=nvs_commit(h);
    nvs_close(h);
    if(e==ESP_OK) {
        using_pending=false;
        ESP_LOGI(TAG,"New router Wi-Fi verified and saved. Previous network replaced.");
    } else ESP_LOGE(TAG,"Could not commit Wi-Fi settings: %s",esp_err_to_name(e));
}
static void wifi_handler(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG,"Wi-Fi STA: connecting...");
        esp_wifi_connect();
    } else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        got_ip=false;
        strlcpy(ip_string,"0.0.0.0",sizeof(ip_string));
        ESP_LOGW(TAG,"Wi-Fi STA disconnected; reconnecting");
        esp_wifi_connect();
    } else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got=(const ip_event_got_ip_t*)data;
        snprintf(ip_string,sizeof(ip_string),IPSTR,IP2STR(&got->ip_info.ip));
        got_ip=true;
        promote_pending_wifi();
        ESP_LOGI(TAG,"AutoMesh web panel ready: http://%s/",ip_string);
    }
}
static void pending_wifi_watchdog(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(45000));
    if(using_pending && !got_ip) {
        nvs_handle_t h;
        esp_err_t e=nvs_open("meshweb",NVS_READWRITE,&h);
        if(e==ESP_OK) {
            e=nvs_erase_key(h,"pend_wifi");
            if(e==ESP_OK)e=nvs_commit(h);
            nvs_close(h);
        }
        if(e==ESP_OK) {
            ESP_LOGW(TAG,"Wi-Fi connection failed: restoring previous network or setup AP");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else ESP_LOGE(TAG,"Wi-Fi recovery failed: %s",esp_err_to_name(e));
    }
    vTaskDelete(NULL);
}
static void delayed_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1400));
    esp_restart();
}
static esp_err_t store_home_network(const char *ssid,const char *password)
{
    if(!valid_network(ssid,password))return ESP_ERR_INVALID_ARG;
    wifi_candidate_t candidate={0};
    strlcpy(candidate.ssid,ssid,sizeof(candidate.ssid));
    strlcpy(candidate.password,password,sizeof(candidate.password));
    nvs_handle_t h;
    esp_err_t e=nvs_open("meshweb",NVS_READWRITE,&h);
    if(e!=ESP_OK)return e;
    e=nvs_set_blob(h,"pend_wifi",&candidate,sizeof(candidate));
    if(e==ESP_OK)e=nvs_commit(h);
    nvs_close(h);
    memset(&candidate,0,sizeof(candidate));
    return e;
}

static bool constant_time_eq(const char *a,const char *b,size_t n)
{
    uint8_t diff=0;
    for(size_t i=0;i<n;i++)diff|=(uint8_t)(a[i]^b[i]);
    return diff==0;
}
static bool authorize(httpd_req_t *req)
{
    char provided[ADMIN_LEN+2]={0};
    if(httpd_req_get_hdr_value_str(req,"X-Automesh-Admin",provided,sizeof(provided))!=ESP_OK ||
       strlen(provided)!=ADMIN_LEN ||
       !constant_time_eq(provided,admin_key,ADMIN_LEN)) {
        httpd_resp_send_err(req,HTTPD_401_UNAUTHORIZED,"Admin token required");
        return false;
    }
    return true;
}
static esp_err_t json_reply(httpd_req_t *req,const char *status,const char *json)
{
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"application/json; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    return httpd_resp_sendstr(req,json);
}
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Frame-Options","DENY");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Content-Security-Policy",
                       "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req,gateway_index_html_start,
                            gateway_index_html_end-gateway_index_html_start);
}
static esp_err_t list_handler(httpd_req_t *req)
{
    if(!authorize(req))return ESP_OK;
    char *buf=malloc(8000);
    if(!buf)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"No memory");
    int n=automesh_devices_json(buf,8000);
    if(n<0) {
        free(buf);
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Mesh busy");
    }
    esp_err_t e=json_reply(req,"200 OK",buf);
    free(buf);
    return e;
}
static esp_err_t status_handler(httpd_req_t *req)
{
    if(!authorize(req))return ESP_OK;
    cJSON *doc=cJSON_CreateObject();
    if(!doc)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"No memory");
    cJSON_AddStringToObject(doc,"version","0.3.0-dev");
    cJSON_AddStringToObject(doc,"ssid",setup_mode?ap_ssid:sta_ssid);
    cJSON_AddStringToObject(doc,"ip",ip_string);
    cJSON_AddStringToObject(doc,"mode",setup_mode?"setup_ap":"sta");
    cJSON_AddBoolToObject(doc,"setup_mode",setup_mode);
    cJSON_AddBoolToObject(doc,"mesh_ota_ready",false);
    char *json=cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if(!json)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"No memory");
    esp_err_t e=json_reply(req,"200 OK",json);
    free(json);
    return e;
}
static bool valid_origin(httpd_req_t *req)
{
    char origin[96]={0};
    if(httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK)
        return true; // Still requires admin token. Native clients may omit Origin.
    char host[70]={0},expected[100]={0};
    if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK)
        return false;
    snprintf(expected,sizeof(expected),"http://%s",host);
    return strcmp(origin,expected)==0;
}
static cJSON *read_post_json(httpd_req_t *req)
{
    if(req->content_len==0 || req->content_len>MAX_BODY)return NULL;
    char body[MAX_BODY+1];
    size_t offset=0;
    while(offset<req->content_len) {
        int n=httpd_req_recv(req,body+offset,req->content_len-offset);
        if(n<=0)return NULL;
        offset+=(size_t)n;
    }
    body[offset]=0;
    return cJSON_ParseWithLength(body,offset);
}


static esp_err_t wifi_scan_status_handler(httpd_req_t *req)
{
    if(!authorize(req))return ESP_OK;
    char *body=automesh_wifi_scan_json();
    if(!body)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"No memory");
    esp_err_t result=json_reply(req,"200 OK",body);
    free(body);
    return result;
}
static esp_err_t wifi_scan_begin_handler(httpd_req_t *req)
{
    if(!authorize(req))return ESP_OK;
    if(!valid_origin(req))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Invalid Origin");
    esp_err_t result=automesh_wifi_scan_begin();
    if(result==ESP_OK)return json_reply(req,"202 Accepted","{\"scanning\":true}");
    if(result==ESP_ERR_INVALID_STATE)
        return json_reply(req,"409 Conflict","{\"error\":\"scan_in_progress\"}");
    char error[100];
    snprintf(error,sizeof(error),"{\"error\":\"%s\"}",esp_err_to_name(result));
    return json_reply(req,"500 Internal Server Error",error);
}

static esp_err_t wifi_setup_handler(httpd_req_t *req)
{
    if(!authorize(req))return ESP_OK;
    if(!valid_origin(req))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Invalid Origin");
    cJSON *body=read_post_json(req);
    if(!body)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid JSON");
    cJSON *name=cJSON_GetObjectItemCaseSensitive(body,"ssid");
    cJSON *pass=cJSON_GetObjectItemCaseSensitive(body,"password");
    esp_err_t e=ESP_ERR_INVALID_ARG;
    if(cJSON_IsString(name) && cJSON_IsString(pass) &&
       name->valuestring && pass->valuestring)
        e=store_home_network(name->valuestring,pass->valuestring);
    cJSON_Delete(body);
    if(e!=ESP_OK) return json_reply(req,"400 Bad Request",
                                    "{\"error\":\"invalid_wifi_credentials\"}");
    ESP_LOGI(TAG,"Candidate Wi-Fi settings saved; restarting with automatic rollback if connection fails");
    esp_err_t result=json_reply(req,"202 Accepted","{\"saved\":true,\"restarting\":true}");
    xTaskCreate(delayed_restart,"restart",2048,NULL,4,NULL);
    return result;
}


static esp_err_t action_handler(httpd_req_t *req)
{
    if(!authorize(req))return ESP_OK;
    if(setup_mode)return json_reply(req,"409 Conflict","{\"error\":\"wifi_setup_required\"}");
    if(!valid_origin(req))return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Invalid Origin");
    cJSON *body=read_post_json(req);
    if(!body)return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid JSON");
    esp_err_t result=ESP_ERR_INVALID_ARG;
    if(strcmp(req->uri,"/api/locate")==0) {
        cJSON *a=cJSON_GetObjectItemCaseSensitive(body,"address");
        cJSON *n=cJSON_GetObjectItemCaseSensitive(body,"seconds");
        if(cJSON_IsNumber(a) && a->valuedouble==(double)a->valueint &&
           a->valueint>0 && a->valueint<0x8000 &&
           cJSON_IsNumber(n) && n->valuedouble==(double)n->valueint &&
           n->valueint>=1 && n->valueint<=30)
            result=automesh_gateway_locate((uint16_t)a->valueint,(uint8_t)n->valueint);
    } else if(strcmp(req->uri,"/api/ping")==0) {
        cJSON *a=cJSON_GetObjectItemCaseSensitive(body,"address");
        if(cJSON_IsNumber(a) && a->valueint>0 && a->valueint<0x8000)
            result=automesh_gateway_ping((uint16_t)a->valueint);
    } else {
        cJSON *u=cJSON_GetObjectItemCaseSensitive(body,"uuid");
        if(cJSON_IsString(u) && u->valuestring) {
            if(strcmp(req->uri,"/api/deny")==0) result=automesh_deny_uuid(u->valuestring);
            if(strcmp(req->uri,"/api/approve")==0) {
                cJSON *o=cJSON_GetObjectItemCaseSensitive(body,"oob");
                if(cJSON_IsString(o) && o->valuestring)
                    result=automesh_approve_uuid(u->valuestring,o->valuestring);
            }
        }
    }
    cJSON_Delete(body);
    if(result==ESP_OK)return json_reply(req,"202 Accepted","{\"accepted\":true}");
    char out[130];
    snprintf(out,sizeof(out),"{\"error\":\"%s\"}",esp_err_to_name(result));
    return json_reply(req,result==ESP_ERR_INVALID_STATE?"409 Conflict":"400 Bad Request",out);
}

esp_err_t automesh_web_start(void)
{
    esp_err_t e=load_credentials();
    if(e!=ESP_OK)return e;
    setup_mode=!load_home_network();
    uint8_t mac[6]={0};
    e=esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP);
    if(e!=ESP_OK)return e;
    snprintf(ap_ssid,sizeof(ap_ssid),"AutoMesh-%02X%02X%02X",mac[3],mac[4],mac[5]);

    e=esp_netif_init();
    if(e!=ESP_OK && e!=ESP_ERR_INVALID_STATE)return e;
    e=esp_event_loop_create_default();
    if(e!=ESP_OK && e!=ESP_ERR_INVALID_STATE)return e;

    if(setup_mode) {
        wifi_interface=esp_netif_create_default_wifi_ap();
        // STA interface is needed to scan while the setup AP remains visible.
        esp_netif_t *scan_station=esp_netif_create_default_wifi_sta();
        if(!scan_station)return ESP_ERR_NO_MEM;
        strlcpy(ip_string,"192.168.4.1",sizeof(ip_string));
    } else {
        wifi_interface=esp_netif_create_default_wifi_sta();
        esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&wifi_handler,NULL);
        esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&wifi_handler,NULL);
    }
    if(!wifi_interface)return ESP_ERR_NO_MEM;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    e=esp_wifi_init(&init);
    if(e!=ESP_OK)return e;
    e=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(e!=ESP_OK)return e;
    e=esp_wifi_set_mode(setup_mode?WIFI_MODE_APSTA:WIFI_MODE_STA);
    if(e!=ESP_OK)return e;
    wifi_config_t cfg={0};
    if(setup_mode) {
        strlcpy((char*)cfg.ap.ssid,ap_ssid,sizeof(cfg.ap.ssid));
        strlcpy((char*)cfg.ap.password,ap_password,sizeof(cfg.ap.password));
        cfg.ap.ssid_len=strlen(ap_ssid);
        cfg.ap.channel=1;
        cfg.ap.max_connection=4;
        cfg.ap.authmode=WIFI_AUTH_WPA2_PSK;
        cfg.ap.ssid_hidden=0;
        cfg.ap.beacon_interval=100;
        e=esp_wifi_set_config(WIFI_IF_AP,&cfg);
    } else {
        memcpy(cfg.sta.ssid,sta_ssid,strlen(sta_ssid));
        memcpy(cfg.sta.password,sta_pass,strlen(sta_pass));
        cfg.sta.threshold.authmode=sta_pass[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
        e=esp_wifi_set_config(WIFI_IF_STA,&cfg);
    }
    if(e!=ESP_OK)return e;
    e=esp_wifi_start();
    if(e!=ESP_OK)return e;
    if(!setup_mode && using_pending)
        xTaskCreate(pending_wifi_watchdog,"wifi_recover",3072,NULL,4,NULL);
    e=automesh_wifi_scan_init();
    if(e!=ESP_OK)return e;
    httpd_config_t hc=HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers=12;
    hc.stack_size=6144;
    hc.lru_purge_enable=true;
    e=httpd_start(&server,&hc);
    if(e!=ESP_OK)return e;

    httpd_uri_t index={.uri="/",.method=HTTP_GET,.handler=index_handler};
    httpd_uri_t devices={.uri="/api/devices",.method=HTTP_GET,.handler=list_handler};
    httpd_uri_t status={.uri="/api/status",.method=HTTP_GET,.handler=status_handler};
    httpd_uri_t approve={.uri="/api/approve",.method=HTTP_POST,.handler=action_handler};
    httpd_uri_t deny={.uri="/api/deny",.method=HTTP_POST,.handler=action_handler};
    httpd_uri_t ping={.uri="/api/ping",.method=HTTP_POST,.handler=action_handler};
    httpd_uri_t locate={.uri="/api/locate",.method=HTTP_POST,.handler=action_handler};
    httpd_uri_t wifi={.uri="/api/wifi",.method=HTTP_POST,.handler=wifi_setup_handler};
    httpd_uri_t wifi_scan={.uri="/api/wifi/scan",.method=HTTP_POST,.handler=wifi_scan_begin_handler};
    httpd_uri_t wifi_list={.uri="/api/wifi/networks",.method=HTTP_GET,.handler=wifi_scan_status_handler};
    e=httpd_register_uri_handler(server,&index);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&devices);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&status);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&approve);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&deny);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&ping);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&locate);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&wifi);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&wifi_scan);
    if(e==ESP_OK)e=httpd_register_uri_handler(server,&wifi_list);
    if(e!=ESP_OK)return e;

    if(setup_mode) {
        // Printed only to physical USB console; no secrets enter browser until authenticated.
        ESP_LOGI(TAG,"WiFi AP: %s",ap_ssid);
        ESP_LOGI(TAG,"WiFi password: %s",ap_password);
        ESP_LOGI(TAG,"Admin token: %s",admin_key);
        ESP_LOGI(TAG,"SETUP ONLY: Mesh disabled until station Wi-Fi is configured");
        ESP_LOGI(TAG,"Wi-Fi scan available from browser; setup uses AP+STA without Mesh");
        ESP_LOGI(TAG,"Open http://192.168.4.1/ to configure router Wi-Fi");
    } else {
        ESP_LOGI(TAG,"STA+MESH mode started. Router network: %s",sta_ssid);
        ESP_LOGI(TAG,"Admin token: %s",admin_key);
        ESP_LOGI(TAG,"Waiting for Wi-Fi IP address from router");
    }
    return ESP_OK;
}

