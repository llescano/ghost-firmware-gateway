idf_component_register(
    SRCS "src/wifi_provisioner.c" "src/http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event device_identity wifi_manager json supabase_client esp_http_server lwip esp_timer
)
