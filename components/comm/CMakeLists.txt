# components/comm/CMakeLists.txt
# Componente de comunicación ESP-Now para el Gateway

idf_component_register(SRCS "comm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi nvs_flash main json)
