#include "transport.h"
#include "config.h"
#include "sentinel_wire.h"
#include "event_queue.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "transport";

#define TRANSPORT_TOPIC  "sentinel/events/" DEVICE_ID
#define TRANSPORT_MAX_PAYLOAD  512
#define TRANSPORT_QOS  1
#define TRANSPORT_RETAIN  0

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t        s_evt   = NULL;
#define BIT_CONNECTED  BIT0

static TaskHandle_t s_drainer = NULL;

bool transport_is_connected(void)
{
    if (!s_evt) return false;
    return (xEventGroupGetBits(s_evt) & BIT_CONNECTED) != 0;
}

// Low-level frame publication. Does not touch the queue.
static esp_err_t publish_frame(const uint8_t *frame, size_t len)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_publish(s_client, TRANSPORT_TOPIC,
                                         (const char *)frame, (int)len,
                                         TRANSPORT_QOS, TRANSPORT_RETAIN);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

// Drainer runs forever. Sleeps while MQTT is disconnected; wakes up on
// CONNECTED and sends everything that has accumulated.
static void drainer_task(void *arg)
{
    uint8_t frame[EVENT_QUEUE_MAX_FRAME];
    for (;;) {
        // Wait for CONNECTED.
        xEventGroupWaitBits(s_evt, BIT_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);

        // Drain the queue while connected and there is data to send.
        while (transport_is_connected()) {
            size_t n = event_queue_peek(frame, sizeof(frame));
            if (n == 0) break;  // queue is empty

            if (publish_frame(frame, n) == ESP_OK) {
                event_queue_pop();
            } else {
                // Publish failed - likely disconnected. Wait for
                // CONNECTED again. Frame stays in queue - not lost.
                ESP_LOGW(TAG, "drain publish failed, will retry");
                break;
            }
        }

        // If queue is empty - sleep until next CONNECTED cycle
        // (or new push; waking drainer from push is not required,
        // because push only happens when MQTT is already connected -
        // then transport_send publishes directly).
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "mqtt connected");
            xEventGroupSetBits(s_evt, BIT_CONNECTED);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "mqtt disconnected");
            xEventGroupClearBits(s_evt, BIT_CONNECTED);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "mqtt error");
            break;
        default:
            break;
    }
}

esp_err_t transport_init(void)
{
    if (s_client) return ESP_OK;

    event_queue_init();

    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "mqtt init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));

    EventBits_t bits = xEventGroupWaitBits(s_evt, BIT_CONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(10000));
    if (!(bits & BIT_CONNECTED)) {
        ESP_LOGE(TAG, "mqtt did not connect in 10s");
        // Do not return error - give drainer a chance to send accumulated data later.
    }

    // Start drainer ONCE. Stack 4096 - it can publish frames
    // up to 128 bytes and run the event loop, comfortable margin.
    xTaskCreate(drainer_task, "tx_drainer", 4096, NULL, 4, &s_drainer);

    ESP_LOGI(TAG, "transport ready, topic=%s", TRANSPORT_TOPIC);
    return ESP_OK;
}

esp_err_t transport_send(sentinel_event_type_t event_type,
                         const uint8_t *payload, size_t payload_len)
{
    if (!s_evt) return ESP_ERR_INVALID_STATE;
    if (payload_len > TRANSPORT_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    if (payload_len > 0 && !payload)         return ESP_ERR_INVALID_ARG;

    // Build wire-frame on the stack.
    uint8_t frame[SENTINEL_HEADER_SIZE + TRANSPORT_MAX_PAYLOAD];
    size_t hlen = sentinel_write_header(frame, sizeof(frame),
                                        event_type, (uint16_t)payload_len);
    if (hlen == 0) return ESP_ERR_INVALID_SIZE;
    if (payload_len) memcpy(&frame[hlen], payload, payload_len);
    size_t total = hlen + payload_len;

    // Hot path: if connected - publish immediately, without queue.
    // This preserves old behavior and does not add latency.
    if (transport_is_connected()) {
        esp_err_t err = publish_frame(frame, total);
        if (err == ESP_OK) return ESP_OK;
        // Publish failed (can happen on race with disconnect). Fall back to queue.
        ESP_LOGW(TAG, "publish failed, queueing instead");
    }

    // Cold path: no connection - put in queue. Drainer will take it on reconnect.
    if (!event_queue_push(frame, total)) {
        ESP_LOGE(TAG, "queue push failed for event_type=%u len=%u",
                 event_type, (unsigned)total);
        return ESP_FAIL;
    }
    return ESP_OK;
}