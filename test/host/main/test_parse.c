#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_fixture.h"
#include "test_helpers.h"
#include "config_manager_priv.h"

TEST_GROUP(parse);

TEST_SETUP(parse) { test_env_setup(); }
TEST_TEAR_DOWN(parse) { test_env_teardown(); }

// Saves a one-unit, one-IO config with the given extra IO fields and returns the IO's function codes.
static void io_codes(const char *io_fields, uint8_t *read_fc, uint8_t *write_fc)
{
    char json[512];
    snprintf(json, sizeof(json),
             "{\"config_version\":1,\"device_ID\":\"d\",\"name\":\"n\",\"units\":{\"u\":{\"unit_name\":\"x\","
             "\"unit_address\":5,\"unit_default_address\":5,\"unit_wifi_interval\":1,\"unit_NB-IOT_interval\":1,"
             "\"IO_config\":{\"io\":{\"IO_name\":\"io\",\"IO_read_address\":1,\"IO_start_byte\":0,\"IO_length\":2,"
             "\"IO_value_type\":\"UINT16\",\"IO_unit\":\"\"%s%s}}}}}",
             io_fields[0] ? "," : "", io_fields);
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(json));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(1, cfg->io_pool_count);
    *read_fc = cfg->io_pool[0].read_fc;
    *write_fc = cfg->io_pool[0].write_fc;
    config_manager_release();
}

#define ASSERT_CODES(fields, r, w) do { uint8_t rf, wf; io_codes(fields, &rf, &wf); \
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r, rf, fields); TEST_ASSERT_EQUAL_UINT8_MESSAGE(w, wf, fields); } while (0)

TEST(parse, example_config)
{
    load_example();
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(1, cfg->version);
    TEST_ASSERT_EQUAL_STRING("MIOT-Gateway", cfg->name);
    TEST_ASSERT_EQUAL_STRING("8574655FD2B55DF0FFBDC496B91CEF5F", cfg->device_id);
    TEST_ASSERT_EQUAL(3, cfg->unit_count);
    TEST_ASSERT_EQUAL(5, cfg->io_pool_count);
    TEST_ASSERT_EQUAL(3, cfg->configure_unit_count);

    // Units are sorted by key during load
    TEST_ASSERT_EQUAL_STRING("unit_2", cfg->units[1].name);
    TEST_ASSERT_EQUAL_UINT8(20, cfg->units[1].address);
    TEST_ASSERT_EQUAL_UINT16(10, cfg->units[1].wifi_interval_s);
    TEST_ASSERT_EQUAL_UINT16(600, cfg->units[1].nbiot_interval_s);
    TEST_ASSERT_EQUAL(3, cfg->units[1].io_count);

    const io_config_t *level = find_io(cfg, "unit_1", "level");
    TEST_ASSERT_NOT_NULL(level);
    TEST_ASSERT_EQUAL_UINT16(100, level->read_addr);
    TEST_ASSERT_EQUAL_UINT8(4, level->length);
    TEST_ASSERT_EQUAL(IO_VTYPE_FP32, level->value_type);
    TEST_ASSERT_TRUE(level->big_endian);
    TEST_ASSERT_EQUAL_UINT8(3, level->read_fc);
    TEST_ASSERT_EQUAL_UINT8(0, level->write_fc);
    TEST_ASSERT_TRUE(isnan(level->live_value));

    const io_config_t *voltage = find_io(cfg, "unit_2", "voltage");
    TEST_ASSERT_EQUAL_UINT8(4, voltage->read_fc);
    TEST_ASSERT_EQUAL_UINT8(0, voltage->write_fc);

    const io_config_t *relay = find_io(cfg, "unit_2", "relay");
    TEST_ASSERT_EQUAL_UINT8(3, relay->read_fc);
    TEST_ASSERT_EQUAL_UINT8(6, relay->write_fc);
    TEST_ASSERT_EQUAL(IO_VTYPE_UINT16, relay->value_type);

    const io_config_t *temp = find_io(cfg, "unit_3", "temperature");
    TEST_ASSERT_EQUAL(IO_VTYPE_INT16, temp->value_type);
    TEST_ASSERT_EQUAL_UINT8(3, temp->read_fc);
    TEST_ASSERT_EQUAL_UINT8(6, temp->write_fc);

    TEST_ASSERT_EQUAL_STRING("MIOT_Level_Sensor", cfg->configure_units[0].sensor_type);
    TEST_ASSERT_EQUAL_UINT16(298, cfg->configure_units[0].address_register);
    config_manager_release();
}

TEST(parse, function_codes_schema_v1)
{
    ASSERT_CODES("\"IO_function_code\":[3]", 3, 0);
    ASSERT_CODES("\"IO_function_code\":[4]", 4, 0);
    ASSERT_CODES("\"IO_function_code\":[6]", 0, 6);
    ASSERT_CODES("\"IO_function_code\":[5]", 0, 5);
    ASSERT_CODES("\"IO_function_code\":[1,5]", 1, 5);
    ASSERT_CODES("\"IO_function_code\":[16,3]", 3, 16);
    ASSERT_CODES("", 3, 6);
}

TEST(parse, function_codes_win_over_legacy_io_type)
{
    ASSERT_CODES("\"IO_function_code\":[4],\"IO_type\":\"W\"", 4, 0);
}

TEST(parse, invalid_function_codes_fall_back_to_defaults)
{
    ASSERT_CODES("\"IO_function_code\":[]", 3, 6);
    ASSERT_CODES("\"IO_function_code\":[7]", 3, 6);
}

TEST(parse, legacy_io_type)
{
    ASSERT_CODES("\"IO_type\":\"R\"", 3, 0);
    ASSERT_CODES("\"IO_type\":\"W\"", 0, 6);
    ASSERT_CODES("\"IO_type\":4", 4, 0);
    ASSERT_CODES("\"IO_type\":\"16\"", 0, 16);
}

TEST(parse, value_types_and_endian)
{
    const char *base = "{\"config_version\":1,\"units\":{\"u\":{\"unit_address\":2,\"IO_config\":{"
                       "\"a\":{\"IO_name\":\"a\",\"IO_value_type\":\"UINT32\",\"IO_endian\":\"little\"},"
                       "\"b\":{\"IO_name\":\"b\",\"IO_value_type\":\"INT32\"},"
                       "\"c\":{\"IO_name\":\"c\",\"IO_value_type\":\"FLOAT32\"},"
                       "\"d\":{\"IO_name\":\"d\",\"IO_value_type\":\"BOOL\"},"
                       "\"e\":{\"IO_name\":\"e\"}}}}}";
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(base));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(IO_VTYPE_UINT32, find_io(cfg, "u", "a")->value_type);
    TEST_ASSERT_FALSE(find_io(cfg, "u", "a")->big_endian);
    TEST_ASSERT_EQUAL(IO_VTYPE_INT32, find_io(cfg, "u", "b")->value_type);
    TEST_ASSERT_TRUE(find_io(cfg, "u", "b")->big_endian);
    TEST_ASSERT_EQUAL(IO_VTYPE_FP32, find_io(cfg, "u", "c")->value_type);
    TEST_ASSERT_EQUAL(IO_VTYPE_BOOL, find_io(cfg, "u", "d")->value_type);
    TEST_ASSERT_EQUAL(IO_VTYPE_UINT16, find_io(cfg, "u", "e")->value_type);
    TEST_ASSERT_EQUAL_UINT8(2, find_io(cfg, "u", "e")->length);
    TEST_ASSERT_EQUAL_UINT16(60, cfg->units[0].wifi_interval_s);
    TEST_ASSERT_EQUAL_UINT16(300, cfg->units[0].nbiot_interval_s);
    config_manager_release();
}

TEST(parse, set_address_io_is_skipped)
{
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(
        "{\"config_version\":1,\"units\":{\"u\":{\"IO_config\":{"
        "\"Set_Address\":{\"IO_read_address\":298},\"x\":{\"IO_name\":\"x\"}}}}}"));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(1, cfg->io_pool_count);
    TEST_ASSERT_EQUAL_STRING("x", cfg->io_pool[0].name);
    config_manager_release();
}

TEST(parse, units_beyond_max_are_ignored)
{
    char json[4096];
    int n = snprintf(json, sizeof(json), "{\"config_version\":1,\"units\":{");
    for (int i = 0; i < MAX_UNITS + 2; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s\"u%02d\":{\"IO_config\":{\"x\":{\"IO_name\":\"x\"}}}", i ? "," : "", i);
    }
    snprintf(json + n, sizeof(json) - n, "}}");
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(json));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(MAX_UNITS, cfg->unit_count);
    TEST_ASSERT_EQUAL(MAX_UNITS, cfg->io_pool_count);
    config_manager_release();
}

TEST(parse, invalid_json_is_rejected)
{
    load_example();
    TEST_ASSERT_EQUAL(ESP_FAIL, config_manager_save("{broken"));
    TEST_ASSERT_EQUAL(3, config_manager_get_ptr()->unit_count);
    config_manager_release();
    config_manager_unload();
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    TEST_ASSERT_EQUAL(3, config_manager_get_ptr()->unit_count);
    config_manager_release();
}

TEST(parse, missing_file_writes_defaults)
{
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(1, cfg->version);
    TEST_ASSERT_EQUAL_STRING("MIOT-Gateway", cfg->name);
    TEST_ASSERT_EQUAL(0, cfg->unit_count);
    TEST_ASSERT_EQUAL(2, cfg->configure_unit_count);
    config_manager_release();

    TEST_ASSERT_EQUAL(ESP_OK, config_manager_reset_to_default("GW-TEST-1"));
    TEST_ASSERT_EQUAL_STRING("GW-TEST-1", config_manager_get_string("device_ID"));

    // Hand the generated defaults to schema_check.py
    char *json = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_NOT_NULL(json);
    FILE *f = fopen(DEFAULTS_OUT, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(json, f);
    fclose(f);
    free(json);
}

TEST(parse, unload_and_get_ptr)
{
    load_example();
    config_manager_unload();
    TEST_ASSERT_NULL(config_manager_get_ptr());
    TEST_ASSERT_EQUAL(0, config_manager_get_version());
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    TEST_ASSERT_EQUAL(3, config_manager_get_ptr()->unit_count);
    config_manager_release();
}

TEST_GROUP_RUNNER(parse)
{
    RUN_TEST_CASE(parse, example_config);
    RUN_TEST_CASE(parse, function_codes_schema_v1);
    RUN_TEST_CASE(parse, function_codes_win_over_legacy_io_type);
    RUN_TEST_CASE(parse, invalid_function_codes_fall_back_to_defaults);
    RUN_TEST_CASE(parse, legacy_io_type);
    RUN_TEST_CASE(parse, value_types_and_endian);
    RUN_TEST_CASE(parse, set_address_io_is_skipped);
    RUN_TEST_CASE(parse, units_beyond_max_are_ignored);
    RUN_TEST_CASE(parse, invalid_json_is_rejected);
    RUN_TEST_CASE(parse, missing_file_writes_defaults);
    RUN_TEST_CASE(parse, unload_and_get_ptr);
}
