idf_component_register(
    SRCS "src/config_manager.c" "src/cm_alloc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common espressif__cjson
    PRIV_REQUIRES freertos heap mbedtls log
)
