#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire-format events heap (little-endian), 16 byte:
//   0..7   ts_us            int64   esp_timer_get_time()
//   8..11  free_now         uint32  xPortGetFreeHeapSize()
//   12..15 free_min_ever    uint32  xPortGetMinimumEverFreeHeapSize()
#define HEAP_PAYLOAD_SIZE 16

// Start periodical timer: once in period_s seconds takes heap-metrics
// and sends 1 event. Call ONCE after transport_init().
void heap_event_init(uint32_t period_s);

#ifdef __cplusplus
}
#endif