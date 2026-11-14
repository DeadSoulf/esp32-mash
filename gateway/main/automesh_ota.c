#include "automesh_ota.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG="AUTOMESH_OTA";
#define OTA_CHUNK_BYTES 1024
#define OTA_MIN_IMAGE_BYTES 512

static bool hex_sha256(const char *hex)
{
    if (!hex || strlen(hex)!=64)return false;
    for (int i=0;i<64;i++) {
        const char c=hex[i];
        if(!((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F')))
            return false;
    }
    return true;
}

static bool sha_matches(const uint8_t *bytes,const char *expected)
{
    unsigned mismatch=0;
    const char digits[]="0123456789abcdef";
    for(int i=0;i<32;i++) {
        const char a=digits[bytes[i]>>4],b=digits[bytes[i]&15];
        char x=expected[2*i],y=expected[2*i+1];
        if(x>='A'&&x<='F')x+=(char)('a'-'A');
        if(y>='A'&&y<='F')y+=(char)('a'-'A');
        mismatch |= (unsigned)(a!=x) | (unsigned)(b!=y);
    }
    return mismatch==0;
}

static void restart_ota_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2200));
    esp_restart();
    vTaskDelete(NULL);
}

esp_err_t automesh_ota_upload(httpd_req_t *req)
{
    const esp_partition_t *target=esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(!target || !running || target->address==running->address)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "OTA partitions unavailable");
    if(req->content_len<OTA_MIN_IMAGE_BYTES || req->content_len>target->size) {
        httpd_resp_set_status(req,"413 Payload Too Large");
        httpd_resp_set_type(req,"text/plain");
        return httpd_resp_sendstr(req,"Invalid firmware size for OTA slot");
    }

    char sha_expected[65]={0};
    const size_t sha_len=httpd_req_get_hdr_value_len(req,"X-Image-SHA256");
    if(sha_len>0) {
        if(sha_len!=64 ||
           httpd_req_get_hdr_value_str(req,"X-Image-SHA256",
                                      sha_expected,sizeof(sha_expected))!=ESP_OK ||
           !hex_sha256(sha_expected))
            return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
                                       "Invalid X-Image-SHA256");
    }

    // Validate chip identity from the *binary image*, not the filename.
    esp_image_header_t header={0};
    size_t read_bytes=0;
    while(read_bytes<sizeof(header)) {
        int n=httpd_req_recv(req,((char*)&header)+read_bytes,sizeof(header)-read_bytes);
        if(n<=0) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
                                             "Firmware header incomplete");
        read_bytes+=(size_t)n;
    }
#if CONFIG_IDF_TARGET_ESP32C3
    const uint16_t expected_chip=ESP_CHIP_ID_ESP32C3;
#elif CONFIG_IDF_TARGET_ESP32S3
    const uint16_t expected_chip=ESP_CHIP_ID_ESP32S3;
#else
#error Unsupported AutoMesh gateway OTA chip
#endif
    if(header.magic!=ESP_IMAGE_HEADER_MAGIC || header.chip_id!=expected_chip) {
        ESP_LOGW(TAG,"Rejected foreign firmware (chip ID=%u, expected=%u)",
                 (unsigned)header.chip_id,(unsigned)expected_chip);
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
                                   "Firmware was built for another ESP32 chip");
    }

    esp_ota_handle_t handle=0;
    esp_err_t e=esp_ota_begin(target,req->content_len,&handle);
    if(e!=ESP_OK) {
        ESP_LOGE(TAG,"Cannot begin OTA: %s",esp_err_to_name(e));
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "OTA partition write unavailable");
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool has_sha=mbedtls_sha256_starts(&sha,0)==0;
    if(has_sha)has_sha=mbedtls_sha256_update(&sha,(uint8_t*)&header,sizeof(header))==0;
    if(has_sha)e=esp_ota_write(handle,&header,sizeof(header));
    char buffer[OTA_CHUNK_BYTES];
    size_t remaining=req->content_len-sizeof(header);
    while(e==ESP_OK && has_sha && remaining) {
        int n=httpd_req_recv(req,buffer,
                             remaining<sizeof(buffer)?remaining:sizeof(buffer));
        if(n==HTTPD_SOCK_ERR_TIMEOUT)continue;
        if(n<=0) {
            e=ESP_FAIL;
            break;
        }
        has_sha=mbedtls_sha256_update(&sha,(const unsigned char*)buffer,(size_t)n)==0;
        if(has_sha)e=esp_ota_write(handle,buffer,(size_t)n);
        remaining-=(size_t)n;
    }
    uint8_t digest[32]={0};
    if(has_sha && e==ESP_OK && !remaining)
        has_sha=mbedtls_sha256_finish(&sha,digest)==0;
    mbedtls_sha256_free(&sha);

    if(e!=ESP_OK || !has_sha || remaining) {
        esp_ota_abort(handle);
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
                                   "Incomplete or invalid OTA transfer");
    }
    if(sha_len && !sha_matches(digest,sha_expected)) {
        esp_ota_abort(handle);
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
                                   "Firmware SHA256 does not match supplied digest");
    }
    e=esp_ota_end(handle); // also checks app checksum and image integrity
    if(e!=ESP_OK) {
        ESP_LOGW(TAG,"OTA image validation failed: %s",esp_err_to_name(e));
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
                                   "Invalid app checksum or image");
    }

    esp_app_desc_t new_app={0};
    e=esp_ota_get_partition_description(target,&new_app);
    if(e!=ESP_OK || strncmp(new_app.project_name,
                             esp_app_get_description()->project_name,
                             sizeof(new_app.project_name))!=0) {
        ESP_LOGW(TAG,"Rejected OTA image for different project");
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
                                   "Firmware project mismatch: upload gateway provisioner.bin");
    }
    e=esp_ota_set_boot_partition(target);
    if(e!=ESP_OK)
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not select OTA boot partition");

    ESP_LOGI(TAG,"Verified gateway firmware staged in %s (%u bytes)",
             target->label,(unsigned)req->content_len);
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    const esp_err_t response=httpd_resp_sendstr(req,
                    "{\"ok\":true,\"restarting\":true,\"nvs_preserved\":true}");
    // Reboot only after sending success. No changes to NVS / Mesh credentials.
    BaseType_t started=xTaskCreate(restart_ota_task,"ota_restart",3072,NULL,5,NULL);
    if(started!=pdPASS) ESP_LOGE(TAG,"OTA staged but reboot task failed; restart gateway manually");
    return response;
}

