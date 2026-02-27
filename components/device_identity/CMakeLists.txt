idf_component_register(
    SRCS "src/device_identity.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_system
)
