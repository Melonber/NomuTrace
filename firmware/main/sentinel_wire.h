#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===== Wire-формат =====
// [6-byte header][binary payload]
// Header (little-endian):
//   offset 0..1  magic       = {'S','N'}
//   offset 2     schema_ver  = SENTINEL_SCHEMA_VERSION
//   offset 3     event_type  = sentinel_event_type_t
//   offset 4..5  payload_len = длина payload (НЕ включая заголовок)

#define SENTINEL_MAGIC_0          0x53  // 'S'
#define SENTINEL_MAGIC_1          0x4E  // 'N'
#define SENTINEL_SCHEMA_VERSION   0x01
#define SENTINEL_HEADER_SIZE      6

// Never reuse numbers - only appending
typedef enum {
    SENTINEL_EVT_BOOT             = 1,
    SENTINEL_EVT_SOCKET           = 2,
    SENTINEL_EVT_STACK_WATERMARK  = 3,
	SENTINEL_EVT_HEAP             = 4,
	SENTINEL_EVT_WIFI 			  = 5,
    // registered:
    // SENTINEL_EVT_OTA   = 4,
    // SENTINEL_EVT_WIFI  = 5,
    // SENTINEL_EVT_HEAP  = 6,
    // SENTINEL_EVT_TLS   = 7,
} sentinel_event_type_t;


static inline size_t sentinel_write_header(uint8_t *buf, size_t buf_len,
                                           sentinel_event_type_t type,
                                           uint16_t payload_len)
{
    if (buf_len < SENTINEL_HEADER_SIZE) return 0;
    buf[0] = SENTINEL_MAGIC_0;
    buf[1] = SENTINEL_MAGIC_1;
    buf[2] = SENTINEL_SCHEMA_VERSION;
    buf[3] = (uint8_t) type;
    buf[4] = (uint8_t)(payload_len & 0xff);
    buf[5] = (uint8_t)((payload_len >> 8) & 0xff);
    return SENTINEL_HEADER_SIZE;
}

#ifdef __cplusplus
}
#endif