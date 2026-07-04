#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Wire-format wifi event (little-endian), 24 bytes:
//   0..7    ts_us           int64    esp_timer_get_time()
//   8       sub_event       uint8    1=connected, 2=disconnected
//   9       channel         uint8    Wi-Fi channel (1..14), 0 if not applicable
//   10..11  reason          uint16   reason code (for disconnected only, otherwise 0)
//   12..17  bssid           6 bytes  AP MAC address
//   18      rssi_signed     int8     RSSI dBm (for connected only, otherwise 0)
//   19..23  reserved        5 bytes  (0)
#define WIFI_PAYLOAD_SIZE 24

// Registers WIFI_EVENT_* handler. Call ONCE after transport_init().
// Must be called AFTER wifi_sta_connect(), because it initializes
// esp_event_loop and esp_wifi.
void wifi_event_init(void);

#ifdef __cplusplus
}
#endif