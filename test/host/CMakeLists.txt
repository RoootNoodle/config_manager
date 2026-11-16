cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
# No SPIFFS/LittleFS on linux, so the config file goes to a temp folder on the PC
# REVIEW: v2 file name per Kconfig default; the v1 path is overridden too so its removal can be tested.
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_MANAGER_FILE_PATH=\"/tmp/config_manager_host_test/config_v2.json\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_MANAGER_V1_FILE_PATH=\"/tmp/config_manager_host_test/config.json\"" APPEND)

project(config_manager_host_test)

# IDF 6.0.1 on linux: libmbed-builtin.a needs memory hooks and psa_hash_compute from libtfpsacrypto.a, which is linked earlier
idf_build_get_property(host_test_elf EXECUTABLE)
target_link_options(${host_test_elf} PRIVATE
                    "-Wl,--undefined=esp_mbedtls_mem_calloc"
                    "-Wl,--undefined=esp_mbedtls_mem_free"
                    "-Wl,--undefined=psa_hash_compute")
