idf_component_register(
    SRCS "main.c" "boot_event.c" "socket_event.c" "stack_event.c" "heap_event.c"
         "wifi_event.c" "event_queue.c" "transport.c" "wifi_sta.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_timer app_update esp_app_format lwip esp_event
)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lwip_connect")