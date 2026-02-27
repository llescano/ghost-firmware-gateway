idf_component_register(
    SRCS "src/phoenix_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http esp_http_client json cjson esp_timer
)
