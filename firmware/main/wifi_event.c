#include "wifi_event.h"
#include "sentinel_wire.h"
#include "transport.h"

#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_timer.h"

static const char *TAG = "wifi_event";

#define WIFI_SUB_CONNECTED    1
#define WIFI_SUB_DISCONNECTED 2

static bool s_registered = false;

static void build_payload(uint8_t *out,
                          int64_t ts_us,
                          uint8_t sub_event,
                          uint8_t channel,
                          uint16_t reason,
                          const uint8_t bssid[6],
                          int8_t rssi)
{
    memset(out, 0, WIFI_PAYLOAD_SIZE);

    // ts_us - little-endian int64
    uint64_t ts = (uint64_t) ts_us;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(ts >> (8 * i));

    out[8] = sub_event;
    out[9] = channel;

    // reason - little-endian uint16
    out[10] = (uint8_t)(reason & 0xff);
    out[11] = (uint8_t)((reason >> 8) & 0xff);

    // bssid - 6 bytes as-is (network order, as everyone writes)
    if (bssid) memcpy(&out[12], bssid, 6);

    // rssi - int8 in reinterpreted form
    out[18] = (uint8_t)(int8_t)rssi;

    // out[19..23] already zeroed by memset
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    if (base != WIFI_EVENT) return;

    uint8_t payload[WIFI_PAYLOAD_SIZE];
    int64_t ts_us = esp_timer_get_time();

    if (event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e =
            (const wifi_event_sta_connected_t *) event_data;

        build_payload(payload, ts_us,
                      WIFI_SUB_CONNECTED,
                      e->channel,
                      0,
                      e->bssid,
                      0);  // RSSI not provided in connected event, will query separately

        // Pull RSSI at connection time via API.
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            payload[18] = (uint8_t)(int8_t) ap.rssi;
        }

        ESP_LOGI(TAG, "connected ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                 e->channel, e->bssid[0], e->bssid[1], e->bssid[2],
                 e->bssid[3], e->bssid[4], e->bssid[5]);

        transport_send(SENTINEL_EVT_WIFI, payload, sizeof(payload));

    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e =
            (const wifi_event_sta_disconnected_t *) event_data;

        build_payload(payload, ts_us,
                      WIFI_SUB_DISCONNECTED,
                      0,
                      (uint16_t) e->reason,
                      e->bssid,
                      e->rssi);

        ESP_LOGW(TAG, "disconnected reason=%u rssi=%d "
                      "bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned) e->reason, (int) e->rssi,
                 e->bssid[0], e->bssid[1], e->bssid[2],
                 e->bssid[3], e->bssid[4], e->bssid[5]);

        transport_send(SENTINEL_EVT_WIFI, payload, sizeof(payload));
    }
    // other events (SCAN_DONE, STA_START, etc.) are ignored for now
}

void wifi_event_init(void)
{
    if (s_registered) return;

    esp_err_t err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &on_wifi_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event handler register failed: %s", esp_err_to_name(err));
        return;
    }
    s_registered = true;
    ESP_LOGI(TAG, "wifi observer ready");
}