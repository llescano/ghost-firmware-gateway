idf_component_register(
    SRCS "src/wifi_provisioner.c" "src/dns_server.c" "src/http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event device_identity wifi_manager json cJSON
)
