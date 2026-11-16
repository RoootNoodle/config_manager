idf_component_register(SRCS "test_main.c" "test_helpers.c" "test_parse.c" "test_hash.c" "test_update.c"
                       PRIV_INCLUDE_DIRS "../../../src"
                       PRIV_REQUIRES config_manager unity freertos)

# REVIEW: tests run against the v2 example; schemas/v1 stays for reference only.
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SCHEMA_DIR=\"${CMAKE_CURRENT_LIST_DIR}/../../../schemas/v2\"")
