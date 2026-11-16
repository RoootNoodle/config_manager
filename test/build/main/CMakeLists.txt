# esp_psram pulls in the SPIRAM Kconfig; COMPONENTS main would otherwise trim it from the build
idf_component_register(SRCS "main.c" PRIV_REQUIRES config_manager esp_psram)
