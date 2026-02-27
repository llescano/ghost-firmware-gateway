# main/CMakeLists.txt - firmware_gateway
# Registro del componente principal del Gateway

idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "includes"
                       REQUIRES controller comm ui nvs_flash esp_wifi esp_driver_gpio wifi_manager supabase_client realtime_commands)
