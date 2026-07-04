#include "stack_event.h"
#include "sentinel_wire.h"
#include "transport.h"

#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "stack_event";

static esp_timer_handle_t s_timer = NULL;

// One event serialization. payload[28].
static void build_payload(uint8_t *out, int64_t ts_us, uint32_t watermark_words,
                          uint32_t stack_size_est, const char *task_name)
{
    // ts_us — little-endian int64
    uint64_t ts = (uint64_t) ts_us;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(ts >> (8 * i));

    // watermark — little-endian uint32
    out[8]  = (uint8_t)(watermark_words & 0xff);
    out[9]  = (uint8_t)((watermark_words >> 8) & 0xff);
    out[10] = (uint8_t)((watermark_words >> 16) & 0xff);
    out[11] = (uint8_t)((watermark_words >> 24) & 0xff);

    // stack_size_est — little-endian uint32
    out[12] = (uint8_t)(stack_size_est & 0xff);
    out[13] = (uint8_t)((stack_size_est >> 8) & 0xff);
    out[14] = (uint8_t)((stack_size_est >> 16) & 0xff);
    out[15] = (uint8_t)((stack_size_est >> 24) & 0xff);

    // task_name — 12 байт, NUL-padded
    memset(&out[16], 0, 12);
    if (task_name) {
        strncpy((char *)&out[16], task_name, 12);
    }
}

// Timer callback. Executed in a context of esp_timer task (NOT ISR).
static void watermark_cb(void *arg)
{
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) return;


    TaskStatus_t *snapshot = pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (!snapshot) {
        ESP_LOGW(TAG, "no memory for snapshot");
        return;
    }

    uint32_t total_runtime_ignored = 0;
    UBaseType_t got = uxTaskGetSystemState(snapshot, task_count, &total_runtime_ignored);

    int64_t ts_us = esp_timer_get_time();
    uint8_t payload[STACK_PAYLOAD_SIZE];

    for (UBaseType_t i = 0; i < got; i++) {
        const TaskStatus_t *t = &snapshot[i];


        uint32_t wm_words = (uint32_t) t->usStackHighWaterMark;


        build_payload(payload, ts_us, wm_words, 0, t->pcTaskName);

        esp_err_t err = transport_send(SENTINEL_EVT_STACK_WATERMARK,
                                       payload, sizeof(payload));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send failed for task '%s': %s",
                     t->pcTaskName, esp_err_to_name(err));
            break; 
        }
    }

    vPortFree(snapshot);
}

void stack_event_init(uint32_t period_s)
{
    if (s_timer) return;

    const esp_timer_create_args_t args = {
        .callback = &watermark_cb,
        .name = "stack_watermark",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer,
                                             (uint64_t)period_s * 1000000ULL));

    ESP_LOGI(TAG, "watermark observer ready, period=%us", (unsigned)period_s);
}