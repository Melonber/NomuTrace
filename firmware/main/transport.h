#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sentinel_wire.h"

esp_err_t transport_init(void);
esp_err_t transport_send(sentinel_event_type_t event_type,
                         const uint8_t *payload, size_t payload_len);

// Returns true if MQTT is currently connected.
// Used by backlog drainer to decide whether it can send.
bool transport_is_connected(void);