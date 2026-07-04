#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "sentinel_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_QUEUE_BUFFER_BYTES  4096
#define EVENT_QUEUE_MAX_FRAME     128    // верхний потолок одной записи

void event_queue_init(void);


bool event_queue_push(const uint8_t *frame, size_t len);


size_t event_queue_peek(uint8_t *buf, size_t buf_len);

void event_queue_pop(void);


uint32_t event_queue_consume_drop_count(void);

size_t event_queue_size(void);

#ifdef __cplusplus
}
#endif