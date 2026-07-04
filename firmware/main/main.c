#include "boot_event.h"
#include "wifi_sta.h"
#include "transport.h"
#include "socket_event.h"
#include "sentinel_wire.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "stack_event.h"
// for testing connects
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "heap_event.h"
#include "wifi_event.h"


static const char *TAG = "main";

// === temporary: testing connect socket_event ===
static void test_connect(const char *ip, uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, ip, &dst.sin_addr);
    connect(s, (struct sockaddr *)&dst, sizeof(dst));  // шим перехватит этот connect()
    close(s);
}

void app_main(void)
{
    boot_event_t ev;
    boot_event_capture(&ev);

    boot_event_start_uptime_tracker();

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    uint8_t boot_payload[BOOT_EVENT_WIRE_SIZE];
    size_t n = boot_event_serialize(&ev, boot_payload, sizeof(boot_payload));
    if (n != BOOT_EVENT_WIRE_SIZE) {
        ESP_LOGE(TAG, "boot_event_serialize failed");
    }

    if (wifi_sta_connect() != ESP_OK) {
        ESP_LOGE(TAG, "no wifi — event was not sent");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (transport_init() != ESP_OK) {
        ESP_LOGE(TAG, "transport_init failed — event was not sent");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (n == BOOT_EVENT_WIRE_SIZE) {
        esp_err_t err = transport_send(SENTINEL_EVT_BOOT, boot_payload, n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "boot send failed: %s", esp_err_to_name(err));
        }
    }


    socket_event_init();
	stack_event_init(30);
	heap_event_init(30);
	wifi_event_init();


    while (1) {
        test_connect("8.8.8.8", 53);        // test
        vTaskDelay(pdMS_TO_TICKS(30000));

    }
}