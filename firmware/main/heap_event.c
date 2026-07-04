#include "heap_event.h"
#include "sentinel_wire.h"
#include "transport.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "heap_event";

static esp_timer_handle_t s_timer = NULL;

static void heap_cb(void *arg)
{
    // Use heap_caps_get_*_size() — native for ESP-IDF API.
    uint32_t free_now      = (uint32_t) heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t free_min_ever = (uint32_t) heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    int64_t ts_us = esp_timer_get_time();

    uint8_t payload[HEAP_PAYLOAD_SIZE];

    // ts_us — little-endian int64
    uint64_t ts = (uint64_t) ts_us;
    for (int i = 0; i < 8; i++) payload[i] = (uint8_t)(ts >> (8 * i));

    // free_now — little-endian uint32
    payload[8]  = (uint8_t)(free_now & 0xff);
    payload[9]  = (uint8_t)((free_now >> 8) & 0xff);
    payload[10] = (uint8_t)((free_now >> 16) & 0xff);
    payload[11] = (uint8_t)((free_now >> 24) & 0xff);

    // free_min_ever — little-endian uint32
    payload[12] = (uint8_t)(free_min_ever & 0xff);
    payload[13] = (uint8_t)((free_min_ever >> 8) & 0xff);
    payload[14] = (uint8_t)((free_min_ever >> 16) & 0xff);
    payload[15] = (uint8_t)((free_min_ever >> 24) & 0xff);

    esp_err_t err = transport_send(SENTINEL_EVT_HEAP, payload, sizeof(payload));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(err));
    }
}

void heap_event_init(uint32_t period_s)
{
    if (s_timer) return;

    const esp_timer_create_args_t args = {
        .callback = &heap_cb,
        .name = "heap_metric",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer,
                                             (uint64_t)period_s * 1000000ULL));

    ESP_LOGI(TAG, "heap observer ready, period=%us", (unsigned)period_s);
}