idf_component_register(
    SRCS "src/region_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_client
        esp_timer
        esp-tls
        freertos
        mbedtls
        nvs_flash
)
