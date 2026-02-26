# CMakeLists.txt - wifi_manager component
# Gestor de conectividad WiFi para Ghost Gateway

idf_component_register(
    SRCS "src/wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash lwip
)
