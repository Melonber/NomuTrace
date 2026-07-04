#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_EVENT_WIRE_SIZE 40  

typedef enum {
    BOOT_FLASH_ENC_DISABLED    = 0,
    BOOT_FLASH_ENC_DEVELOPMENT = 1,
    BOOT_FLASH_ENC_RELEASE     = 2,
    BOOT_FLASH_ENC_UNKNOWN     = 3,
} boot_flash_enc_t;

typedef struct __attribute__((packed)) {
    uint32_t timestamp;        // 4
    uint32_t last_uptime_s;    // 4
    uint8_t  reset_reason;     // 1
    uint8_t  secure_boot;      // 1
    uint8_t  flash_enc_mode;   // 1
    uint8_t  reserved;         // 1
    char     fw_version[28];   // 28
} boot_event_t;                // = 40

void boot_event_capture(boot_event_t *out);
void boot_event_start_uptime_tracker(void);


size_t boot_event_serialize(const boot_event_t *ev, uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif