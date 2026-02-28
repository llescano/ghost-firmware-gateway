# CMakeLists.txt - supabase_client component
# Cliente HTTP para comunicación con Supabase Edge Functions
# Usa esp_tls directamente para control total sobre ALPN, SNI y Certificate Bundle

idf_component_register(
    SRCS "src/supabase_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-tls nvs_flash json lwip esp_netif sntp_sync device_identity
)
