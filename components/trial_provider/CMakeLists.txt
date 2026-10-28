idf_component_register(
    SRCS
        "src/trial_provider.c"
        "src/trial_region.c"
        "src/trial_token.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        region_service
    PRIV_REQUIRES
        claw_http
        claw_endpoint
        claw_utils
        cjson
        efuse
        esp_app_format
        esp_http_client
        esp_security
        esp-tls
        freertos
        mbedtls
)
