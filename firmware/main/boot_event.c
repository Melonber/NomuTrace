#include "boot_event.h"

#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_secure_boot.h"
#include "esp_flash_encrypt.h"
#include "esp_log.h"

static const char *TAG = "boot_event";

#define RTC_MAGIC 0x53454E54u   // 'S','E','N','T'

RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_uptime_s;

static esp_timer_handle_t s_uptime_timer;

static uint8_t map_flash_enc_mode(void)
{
    if (!esp_flash_encryption_enabled()) {
        return BOOT_FLASH_ENC_DISABLED;
    }
    switch (esp_get_flash_encryption_mode()) {
        case ESP_FLASH_ENC_MODE_DEVELOPMENT: return BOOT_FLASH_ENC_DEVELOPMENT;
        case ESP_FLASH_ENC_MODE_RELEASE:     return BOOT_FLASH_ENC_RELEASE;
        default:                             return BOOT_FLASH_ENC_UNKNOWN;
    }
}

void boot_event_capture(boot_event_t *out)
{
    memset(out, 0, sizeof(*out));

    out->timestamp = (uint32_t) time(NULL);

    out->reset_reason = (uint8_t) esp_reset_reason();

    if (s_rtc_magic == RTC_MAGIC) {
        out->last_uptime_s = s_rtc_uptime_s;   
    } else {
        out->last_uptime_s = 0;        
    }

    // firmware version
    const esp_app_desc_t *desc = esp_app_get_description();
    strlcpy(out->fw_version, desc->version, sizeof(out->fw_version));

    // secure boot
    out->secure_boot = esp_secure_boot_enabled() ? 1 : 0;

    // flash encryption
    out->flash_enc_mode = map_flash_enc_mode();

    ESP_LOGI(TAG,
             "boot_event: reset=%u last_uptime=%us fw=%s secure_boot=%u flash_enc=%u ts=%u",
             out->reset_reason, (unsigned) out->last_uptime_s, out->fw_version,
             out->secure_boot, out->flash_enc_mode, (unsigned) out->timestamp);
}

static void uptime_cb(void *arg)
{
    s_rtc_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    s_rtc_magic = RTC_MAGIC;   
}

void boot_event_start_uptime_tracker(void)
{
    const esp_timer_create_args_t args = {
        .callback = &uptime_cb,
        .name = "uptime_marker",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_uptime_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_uptime_timer, 5 * 1000000ULL));
}

size_t boot_event_serialize(const boot_event_t *ev, uint8_t *buf, size_t buf_len)
{
    if (buf_len < BOOT_EVENT_WIRE_SIZE) {
        return 0;
    }
    size_t i = 0;


    buf[i++] = (uint8_t)(ev->timestamp);
    buf[i++] = (uint8_t)(ev->timestamp >> 8);
    buf[i++] = (uint8_t)(ev->timestamp >> 16);
    buf[i++] = (uint8_t)(ev->timestamp >> 24);

    buf[i++] = (uint8_t)(ev->last_uptime_s);
    buf[i++] = (uint8_t)(ev->last_uptime_s >> 8);
    buf[i++] = (uint8_t)(ev->last_uptime_s >> 16);
    buf[i++] = (uint8_t)(ev->last_uptime_s >> 24);

    buf[i++] = ev->reset_reason;
    buf[i++] = ev->secure_boot;
    buf[i++] = ev->flash_enc_mode;
    buf[i++] = ev->reserved;

    memcpy(&buf[i], ev->fw_version, sizeof(ev->fw_version)); // 28 byte
    i += sizeof(ev->fw_version);

    return i; // 40
}