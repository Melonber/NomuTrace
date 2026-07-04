#include "event_queue.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "event_queue";

// Simple ring-byte buffer. Format
//   [uint16_t LE: length] [length bytes: frame]
// length does not include itself.

static uint8_t s_buf[EVENT_QUEUE_BUFFER_BYTES];
static size_t s_head = 0;   // where to write
static size_t s_tail = 0;   // from where to read
static size_t s_used = 0;   // byte used
static uint32_t s_dropped = 0;
static SemaphoreHandle_t s_mutex = NULL;

#define LENGTH_PREFIX_SIZE 2

static inline void wrap_copy_out(uint8_t *dst, size_t offset, size_t len)
{
    if (offset + len <= EVENT_QUEUE_BUFFER_BYTES) {
        memcpy(dst, &s_buf[offset], len);
    } else {
        size_t first = EVENT_QUEUE_BUFFER_BYTES - offset;
        memcpy(dst, &s_buf[offset], first);
        memcpy(dst + first, &s_buf[0], len - first);
    }
}

static inline void wrap_copy_in(size_t offset, const uint8_t *src, size_t len)
{
    if (offset + len <= EVENT_QUEUE_BUFFER_BYTES) {
        memcpy(&s_buf[offset], src, len);
    } else {
        size_t first = EVENT_QUEUE_BUFFER_BYTES - offset;
        memcpy(&s_buf[offset], src, first);
        memcpy(&s_buf[0], src + first, len - first);
    }
}

static void drop_oldest(void)
{
    if (s_used < LENGTH_PREFIX_SIZE) return;

    uint16_t len = 0;
    wrap_copy_out((uint8_t *)&len, s_tail, LENGTH_PREFIX_SIZE);

    size_t total = LENGTH_PREFIX_SIZE + len;
    s_tail = (s_tail + total) % EVENT_QUEUE_BUFFER_BYTES;
    s_used -= total;
    s_dropped++;
}

void event_queue_init(void)
{
    if (s_mutex) return;
    s_mutex = xSemaphoreCreateMutex();
    s_head = s_tail = s_used = 0;
    s_dropped = 0;
    ESP_LOGI(TAG, "ready, capacity=%d bytes", EVENT_QUEUE_BUFFER_BYTES);
}

bool event_queue_push(const uint8_t *frame, size_t len)
{
    if (!s_mutex) return false;
    if (len == 0 || len > EVENT_QUEUE_MAX_FRAME) return false;

    size_t total = LENGTH_PREFIX_SIZE + len;
    if (total > EVENT_QUEUE_BUFFER_BYTES) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Free space
    while (s_used + total > EVENT_QUEUE_BUFFER_BYTES) {
        drop_oldest();
    }

    uint16_t length_le = (uint16_t) len;
    wrap_copy_in(s_head, (const uint8_t *)&length_le, LENGTH_PREFIX_SIZE);
    s_head = (s_head + LENGTH_PREFIX_SIZE) % EVENT_QUEUE_BUFFER_BYTES;

    wrap_copy_in(s_head, frame, len);
    s_head = (s_head + len) % EVENT_QUEUE_BUFFER_BYTES;

    s_used += total;

    xSemaphoreGive(s_mutex);
    return true;
}

size_t event_queue_peek(uint8_t *buf, size_t buf_len)
{
    if (!s_mutex || !buf) return 0;
    size_t result = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_used >= LENGTH_PREFIX_SIZE) {
        uint16_t len = 0;
        wrap_copy_out((uint8_t *)&len, s_tail, LENGTH_PREFIX_SIZE);
        if (len <= buf_len && (size_t)LENGTH_PREFIX_SIZE + len <= s_used) {
            size_t data_start = (s_tail + LENGTH_PREFIX_SIZE) % EVENT_QUEUE_BUFFER_BYTES;
            wrap_copy_out(buf, data_start, len);
            result = len;
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

void event_queue_pop(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_used >= LENGTH_PREFIX_SIZE) {
        uint16_t len = 0;
        wrap_copy_out((uint8_t *)&len, s_tail, LENGTH_PREFIX_SIZE);
        size_t total = LENGTH_PREFIX_SIZE + len;
        if (total <= s_used) {
            s_tail = (s_tail + total) % EVENT_QUEUE_BUFFER_BYTES;
            s_used -= total;
        }
    }
    xSemaphoreGive(s_mutex);
}

uint32_t event_queue_consume_drop_count(void)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t d = s_dropped;
    s_dropped = 0;
    xSemaphoreGive(s_mutex);
    return d;
}

size_t event_queue_size(void)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = s_used;
    xSemaphoreGive(s_mutex);
    return n;
}