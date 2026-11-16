#pragma once

#include "config_manager.h"

#define TEST_DIR      "/tmp/config_manager_host_test"
#define EXAMPLE_JSON  SCHEMA_DIR "/config_example.json"
// Written by the tests, validated against the schema by test/schema_check.py
#define DEFAULTS_OUT  TEST_DIR "/defaults.json"

void  test_env_setup(void);     // clean dir + init
void  test_env_teardown(void);  // deinit
char *read_text_file(const char *path);  // caller free()
void  write_text_file(const char *path, const char *text);
void  load_example(void);       // save schemas/v2/config_example.json as the active config
// REVIEW: units are looked up by their numeric key now.
const io_config_t *find_io(const system_config_t *cfg, uint8_t unit_key, const char *io);
const char *state_str(const cJSON *state, const char *key);      // NULL if absent
const char *state_unit(const cJSON *state, const char *unit_key); // unit hash in state.u, NULL if absent
