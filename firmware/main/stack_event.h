#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire-format event stack_watermark (little-endian), 28 bytes:
//   0..7   ts_us           int64   esp_timer_get_time()
//   8..11  watermark_words uint32  remaining stack in WORDS (as returned by FreeRTOS)
//   12..15 stack_size_est  uint32  0 if unknown (FreeRTOS does not return total)
//   16..27 task_name       12 bytes  ASCII, NUL-padded (FreeRTOS limit 16, trimmed to 12)
#define STACK_PAYLOAD_SIZE 28

// Starts a periodic timer: every period_s seconds, iterates over all
// tasks and sends one event per task. Call ONCE after transport_init().
void stack_event_init(uint32_t period_s);

#ifdef __cplusplus
}
#endif