idf_component_register(
    SRCS "src/phoenix_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client json cjson esp_timer
)
