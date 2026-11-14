#include "automesh_status_led.h"
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#if CONFIG_IDF_TARGET_ESP32C3
#define AUTOMESH_LED_PIN GPIO_NUM_8
#define AUTOMESH_LED_ACTIVE_LOW 1
#else
// Board-specific LED profiles must be added before enabling another target.
#define AUTOMESH_LED_PIN GPIO_NUM_NC
#define AUTOMESH_LED_ACTIVE_LOW 0
#endif

#define LINK_TIMEOUT_MS 35000
static const char *TAG="AUTOMESH_LED";
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/rmt_tx.h"

// Waveshare ESP32-S3-Zero: one WS2812-compatible LED, GPIO21, RGB ordering.
// RMT at 10 MHz: 0=0.3us high/0.9us low; 1=0.9us high/0.3us low.
static rmt_channel_handle_t automesh_rgb_channel;
static rmt_encoder_handle_t automesh_rgb_encoder;
static bool automesh_rgb_ready;

static void automesh_rgb_init(void)
{
    rmt_tx_channel_config_t channel = {
        .gpio_num = GPIO_NUM_21,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&channel, &automesh_rgb_channel);
    if (err != ESP_OK) goto failed;
    rmt_bytes_encoder_config_t bits = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&bits, &automesh_rgb_encoder);
    if (err != ESP_OK) goto failed;
    err = rmt_enable(automesh_rgb_channel);
    if (err != ESP_OK) goto failed;
    automesh_rgb_ready = true;
    ESP_LOGI(TAG, "ESP32-S3-Zero WS2812 RGB ready on GPIO21 (RMT)");
    return;
failed:
    ESP_LOGE(TAG, "ESP32-S3-Zero RGB LED init failed: %s", esp_err_to_name(err));
    // The network must keep working if the LED cannot be initialized.
}

static void automesh_rgb_show(bool lit, bool online, bool locating)
{
    if (!automesh_rgb_ready) return;
    // On: blue means locate, green means online, amber means offline.
    // Waveshare S3-Zero uses RGB rather than the common GRB byte order.
    const uint8_t rgb[3] = {
        lit ? (locating ? 0 : (online ? 0 : 14)) : 0,
        lit ? (locating ? 0 : (online ? 14 : 7)) : 0,
        lit ? (locating ? 18 : 0) : 0,
    };
    const rmt_transmit_config_t config = {.loop_count = 0};
    esp_err_t err = rmt_transmit(automesh_rgb_channel, automesh_rgb_encoder,
                                rgb, sizeof(rgb), &config);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(automesh_rgb_channel, 100);
    if (err != ESP_OK) ESP_LOGW(TAG, "RGB LED update failed: %s", esp_err_to_name(err));
}
#endif

static volatile bool registered = false;
static volatile TickType_t last_rx = 0;
static volatile int64_t locate_end_us = 0;
void automesh_led_locate(uint8_t seconds)
{
    if (seconds < 1) seconds=1;
    if (seconds > 30) seconds=30;
    locate_end_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    ESP_LOGI(TAG,"Physical locate blinking for %u seconds",(unsigned)seconds);
}


bool automesh_led_is_online(void)
{
    TickType_t t = last_rx;
    return registered && t != 0 &&
           (uint32_t)(xTaskGetTickCount() - t) < pdMS_TO_TICKS(LINK_TIMEOUT_MS);
}
void automesh_led_set_registered(bool value)
{
    registered = value;
    last_rx = 0;
}
void automesh_led_mark_alive(void)
{
    if (registered) last_rx = xTaskGetTickCount();
}
static void task(void *arg)
{
    (void)arg;
    uint32_t phase=0;
    while (true) {
        const bool locating = (esp_timer_get_time() < locate_end_us);
        const bool online = automesh_led_is_online();
        // Locate: ~4 Hz (125 ms on + 125 ms off). Offline: 1 Hz (500+500).
        const bool on = locating ? ((phase & 1u)==0u) :
                        (online || (((phase / 4u) & 1u)==0u));
#if CONFIG_IDF_TARGET_ESP32S3
        automesh_rgb_show(on, online, locating);
#else
        if(AUTOMESH_LED_PIN != GPIO_NUM_NC)
            gpio_set_level(AUTOMESH_LED_PIN,AUTOMESH_LED_ACTIVE_LOW ? !on : on);
#endif
        phase++;
        vTaskDelay(pdMS_TO_TICKS(125));
    }
}
void automesh_led_start(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    automesh_rgb_init();
#else
    if (AUTOMESH_LED_PIN != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << AUTOMESH_LED_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io));
        gpio_set_level(AUTOMESH_LED_PIN, AUTOMESH_LED_ACTIVE_LOW ? 1 : 0);
    } else {
        ESP_LOGW(TAG,"No status LED profile for this ESP32 board");
    }
#endif
    ESP_LOGI(TAG,"Status LED configured: GPIO %d, active_low=%d; no link => blink",
             (int)AUTOMESH_LED_PIN,(int)AUTOMESH_LED_ACTIVE_LOW);
    configASSERT(xTaskCreate(task, "mesh_status_led", 2048, NULL, 3, NULL)==pdPASS);
}

