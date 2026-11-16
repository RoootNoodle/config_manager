#pragma once

#include "config_manager.h"

#define TEST_DIR      "/tmp/config_manager_host_test"
#define EXAMPLE_JSON  SCHEMA_DIR "/config_example.json"
// Written by the tests, validated against the schema by test/schema_check.py
#define DEFAULTS_OUT  TEST_DIR "/defaults.json"

void  test_env_setup(void);     // clean dir + init
void  test_env_teardown(void);  // deinit
char *read_text_file(const char *path);  // caller free()
void  load_example(void);       // save schemas/v1/config_example.json as the active config
const io_config_t *find_io(const system_config_t *cfg, const char *unit, const char *io);
