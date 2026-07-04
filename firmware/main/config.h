#pragma once
#include <stdint.h>

// Wi-Fi
#define WIFI_SSID  ""
#define WIFI_PASS  ""

// MQTT-broker of collectior
#define MQTT_BROKER_URI  ""

#define DEVICE_ID  ""

// ===== Allowlist =====
typedef struct { const char *ip; uint16_t port; } allow_entry_t;

#define SENTINEL_ALLOWLIST { \
    { "ip", port }, \
    { "ip", port }, \
}
