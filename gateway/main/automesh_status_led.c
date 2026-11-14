#include "automesh_status_led.h"
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

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
static volatile bool registered = false;
static volatile TickType_t last_rx = 0;

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
    uint32_t phase = 0;
    bool previous = false;
    while (true) {
        bool online = automesh_led_is_online();
        // Blink 1 Hz, 50% duty when offline; steady on if live.
        bool on = online || ((phase & 1u) == 0u);
        if (AUTOMESH_LED_PIN != GPIO_NUM_NC && (on != previous || phase == 0)) {
            gpio_set_level(AUTOMESH_LED_PIN, AUTOMESH_LED_ACTIVE_LOW ? !on : on);
            previous = on;
        }
        phase++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
void automesh_led_start(void)
{
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
    ESP_LOGI(TAG,"Status LED configured: GPIO %d, active_low=%d; no link => blink",
             (int)AUTOMESH_LED_PIN,(int)AUTOMESH_LED_ACTIVE_LOW);
    configASSERT(xTaskCreate(task, "mesh_status_led", 2048, NULL, 3, NULL)==pdPASS);
}

