#include "wifi_sta.h"
#include "config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define INITIAL_MAX_RETRY  10

static EventGroupHandle_t s_eg;
static int  s_initial_retry  = 0;     // initial connection attempt counter
static bool s_ever_connected = false; // flag: have we ever gotten an IP?

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ever_connected) {
            // Already connected at least once → infinite reconnect, as expected
            // for an IoT device. Insert 2-second delay between attempts to
            // avoid spamming the driver and wasting current on constant scans.
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP_LOGW(TAG, "lost connection, reconnecting...");
            esp_wifi_connect();
        } else if (s_initial_retry < INITIAL_MAX_RETRY) {
            // Initial connection still in progress - limited number of attempts.
            s_initial_retry++;
            ESP_LOGW(TAG, "initial connect retry %d/%d",
                     s_initial_retry, INITIAL_MAX_RETRY);
            esp_wifi_connect();
        } else {
            // Initial connection failed entirely - let app_main see the failure.
            xEventGroupSetBits(s_eg, WIFI_FAIL_BIT);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_initial_retry = 0;
        s_ever_connected = true;
        xEventGroupSetBits(s_eg, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_connect(void)
{
    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_evt, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid,     WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\"...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(
        s_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}