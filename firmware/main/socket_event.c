#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"

#include "config.h"
#include "transport.h"
#include "sentinel_wire.h"
#include "socket_event.h"

static const char *TAG = "socket_event";
#define SOCK_QUEUE_LEN 16

enum { TP_TCP = 0, TP_UDP = 1, TP_OTHER = 2 };

// In-flight 
typedef struct {
    uint32_t ip;        // IPv4, network byte order
    uint16_t port;      // host order
    uint8_t  transport; // TP_*
    uint8_t  family;    // AF_INET
    int64_t  ts_us;     // esp_timer monotonic
} sock_evt_t;

static QueueHandle_t s_queue         = NULL;
static TaskHandle_t  s_reporter_task = NULL;

// allowlist from config.h
static const allow_entry_t s_allow[] = SENTINEL_ALLOWLIST;

static bool allowlist_check(uint32_t ip_net, uint16_t port) {
    size_t n = sizeof(s_allow) / sizeof(s_allow[0]);
    for (size_t i = 0; i < n; i++) {
        uint32_t a = ipaddr_addr(s_allow[i].ip);  // network byte order
        if (a == ip_net && (s_allow[i].port == 0 || s_allow[i].port == port))
            return true;
    }
    return false;
}

// Real lwip_connect (linker from --wrap).
extern int __real_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen);

static uint8_t sock_transport(int s) {
    int type = 0; socklen_t len = sizeof(type);
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
        if (type == SOCK_STREAM) return TP_TCP;
        if (type == SOCK_DGRAM)  return TP_UDP;
    }
    return TP_OTHER;
}

int __wrap_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen) {
    if (s_queue != NULL &&
        xTaskGetCurrentTaskHandle() != s_reporter_task &&  
        name != NULL && name->sa_family == AF_INET) {

        const struct sockaddr_in *a = (const struct sockaddr_in *)name;
        sock_evt_t e = {
            .ip        = a->sin_addr.s_addr,
            .port      = ntohs(a->sin_port),
            .transport = sock_transport(s),
            .family    = AF_INET,
            .ts_us     = esp_timer_get_time(),
        };
        if (xQueueSend(s_queue, &e, 0) != pdTRUE)
            ESP_LOGW(TAG, "queue full, dropping connect event");
    }
    return __real_lwip_connect(s, name, namelen);
}

// ===== Binary wire payload for socket_event =====
// Лейаут (стабильный контракт с коллектором), little-endian:
//   0..7   ts_us           int64_t   (monotonic от esp_timer)
//   8..11  ip              uint32_t  (network byte order — как в lwIP)
//   12..13 port            uint16_t  (LE, host order значение)
//   14     transport       uint8_t   (TP_TCP=0, TP_UDP=1, TP_OTHER=2)
//   15     family          uint8_t   (AF_INET)
//   16     allowlisted     uint8_t   (0/1)
//   17..19 reserved        3 байта   (0)
#define SOCK_PAYLOAD_SIZE 20

static size_t build_binary(const sock_evt_t *e, uint8_t *out, size_t len) {
    if (len < SOCK_PAYLOAD_SIZE) return 0;

    bool allow = allowlist_check(e->ip, e->port);

    // ts_us — little-endian int64
    uint64_t ts = (uint64_t) e->ts_us;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(ts >> (8 * i));

    // ip — оставляем network byte order как есть (lwIP уже отдаёт его таким)
    memcpy(&out[8], &e->ip, 4);

    // port — little-endian
    out[12] = (uint8_t)(e->port & 0xff);
    out[13] = (uint8_t)((e->port >> 8) & 0xff);

    out[14] = e->transport;
    out[15] = e->family;
    out[16] = allow ? 1 : 0;
    out[17] = 0; out[18] = 0; out[19] = 0;

    return SOCK_PAYLOAD_SIZE;
}

static void reporter_task(void *arg) {
    s_reporter_task = xTaskGetCurrentTaskHandle();  
    sock_evt_t e;
    uint8_t payload[SOCK_PAYLOAD_SIZE];
    for (;;) {
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) == pdTRUE) {
            size_t n = build_binary(&e, payload, sizeof(payload));
            if (n == 0) { ESP_LOGW(TAG, "serialize failed, dropping"); continue; }
            esp_err_t err = transport_send(SENTINEL_EVT_SOCKET, payload, n);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "transport_send failed: %s", esp_err_to_name(err));
        }
    }
}

void socket_event_init(void) {
    if (s_queue) return;
    s_queue = xQueueCreate(SOCK_QUEUE_LEN, sizeof(sock_evt_t));
    if (!s_queue) { ESP_LOGE(TAG, "queue alloc failed"); return; }
    xTaskCreate(reporter_task, "sentinel_sock", 3072, NULL, 5, &s_reporter_task);
    ESP_LOGI(TAG, "socket observer ready");
}