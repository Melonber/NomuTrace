#pragma once
#include "esp_err.h"

// Blocking Wi-Fi connection. Returns ESP_OK upon getting an IP, otherwise ESP_FAIL (timeout).
esp_err_t wifi_sta_connect(void);