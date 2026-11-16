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

// One unit "1" with one IO "x" whose body is io_body; returns what config_manager_save() returned.
static esp_err_t save_io(const char *io_body)
{
    char json[512];
    snprintf(json, sizeof(json),
             "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"g\",\"u\":{\"1\":{\"n\":\"x\",\"a\":5,\"io\":{\"x\":%s}}}}", io_body);
    return config_manager_save(json);
}

// A minimal valid file with the root fields replaced by root_extra and unit "1" body by unit_body.
static esp_err_t save_with(const char *root_extra, const char *unit_key, const char *unit_body)
{
    char json[512];
    snprintf(json, sizeof(json), "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"g\"%s,\"u\":{\"%s\":%s}}",
             root_extra, unit_key, unit_body);
    return config_manager_save(json);
}

#define UNIT_OK "{\"n\":\"x\",\"a\":5,\"io\":{}}"

TEST(parse, example_config)
{
    load_example();
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(1, cfg->version);
    TEST_ASSERT_EQUAL_STRING("MIOT-Gateway", cfg->name);
    TEST_ASSERT_EQUAL_STRING("8574655FD2B55DF0FFBDC496B91CEF5F", cfg->device_id);
    TEST_ASSERT_FALSE(cfg->gps_enabled);
    TEST_ASSERT_EQUAL(3, cfg->unit_count);
    TEST_ASSERT_EQUAL(5, cfg->io_pool_count);
    TEST_ASSERT_EQUAL(3, cfg->configure_unit_count);

    TEST_ASSERT_EQUAL_UINT8(2, cfg->units[1].key);
    TEST_ASSERT_EQUAL_UINT8(20, cfg->units[1].address);
    TEST_ASSERT_EQUAL_UINT16(10, cfg->units[1].wifi_interval_s);
    TEST_ASSERT_EQUAL_UINT16(600, cfg->units[1].nbiot_interval_s);
    TEST_ASSERT_EQUAL(3, cfg->units[1].io_count);
    // c left out in unit 1: default 300
    TEST_ASSERT_EQUAL_UINT16(300, cfg->units[0].nbiot_interval_s);

    // IOs of a unit are sorted by name
    TEST_ASSERT_EQUAL_STRING("current", cfg->io_pool[cfg->units[1].io_pool_offset].name);
    TEST_ASSERT_EQUAL_STRING("voltage", cfg->io_pool[cfg->units[1].io_pool_offset + 2].name);

    const io_config_t *level = find_io(cfg, 1, "level");
    TEST_ASSERT_NOT_NULL(level);
    TEST_ASSERT_EQUAL_UINT16(100, level->read_addr);
    TEST_ASSERT_EQUAL_UINT8(4, level->length);
    TEST_ASSERT_EQUAL_UINT8(0, level->start_byte);
    TEST_ASSERT_EQUAL(IO_VTYPE_FP32, level->value_type);
    TEST_ASSERT_TRUE(level->big_endian);
    TEST_ASSERT_EQUAL_UINT8(3, level->read_fc);
    TEST_ASSERT_EQUAL_UINT8(0, level->write_fc);
    TEST_ASSERT_TRUE(isnan(level->live_value));

    const io_config_t *relay = find_io(cfg, 2, "relay");
    TEST_ASSERT_EQUAL_UINT8(3, relay->read_fc);
    TEST_ASSERT_EQUAL_UINT8(6, relay->write_fc);
    TEST_ASSERT_EQUAL_UINT8(2, relay->length);
    TEST_ASSERT_EQUAL(IO_VTYPE_UINT16, relay->value_type);

    TEST_ASSERT_EQUAL_UINT8(4, find_io(cfg, 2, "voltage")->read_fc);
    TEST_ASSERT_EQUAL(IO_VTYPE_INT16, find_io(cfg, 3, "temperature")->value_type);

    TEST_ASSERT_EQUAL_STRING("MIOT_Level_Sensor", cfg->configure_units[0].sensor_type);
    TEST_ASSERT_EQUAL_UINT16(298, cfg->configure_units[0].address_register);
    config_manager_release();
}

TEST(parse, every_default)
{
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(
        "{\"s\":2,\"v\":3,\"id\":\"d\",\"n\":\"g\",\"u\":{\"7\":{\"n\":\"x\",\"a\":2,\"io\":{"
        "\"a\":{\"r\":1,\"t\":\"u16\",\"f\":[3]},\"b\":{\"r\":2,\"t\":\"i16\",\"f\":[4]},"
        "\"c\":{\"r\":3,\"t\":\"u32\",\"f\":[3]},\"d\":{\"r\":4,\"t\":\"i32\",\"f\":[3]},"
        "\"e\":{\"r\":5,\"t\":\"f32\",\"f\":[3]},\"f\":{\"r\":6,\"t\":\"bool\",\"f\":[1]}}}}}"));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_FALSE(cfg->gps_enabled);
    TEST_ASSERT_EQUAL(0, cfg->configure_unit_count);
    TEST_ASSERT_EQUAL_UINT8(7, cfg->units[0].key);
    TEST_ASSERT_EQUAL_UINT16(60, cfg->units[0].wifi_interval_s);
    TEST_ASSERT_EQUAL_UINT16(300, cfg->units[0].nbiot_interval_s);
    static const struct { const char *n; io_value_type_t t; uint8_t l; } exp[] = {
        {"a", IO_VTYPE_UINT16, 2}, {"b", IO_VTYPE_INT16, 2}, {"c", IO_VTYPE_UINT32, 4},
        {"d", IO_VTYPE_INT32, 4}, {"e", IO_VTYPE_FP32, 4}, {"f", IO_VTYPE_BOOL, 2},
    };
    for (int i = 0; i < 6; i++) {
        const io_config_t *io = find_io(cfg, 7, exp[i].n);
        TEST_ASSERT_NOT_NULL_MESSAGE(io, exp[i].n);
        TEST_ASSERT_EQUAL_MESSAGE(exp[i].t, io->value_type, exp[i].n);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(exp[i].l, io->length, exp[i].n);
        TEST_ASSERT_EQUAL_UINT8(0, io->start_byte);
        TEST_ASSERT_TRUE(io->big_endian);
    }
    config_manager_release();
}

TEST(parse, explicit_values)
{
    TEST_ASSERT_EQUAL(ESP_OK, save_with(",\"gps\":1,\"cu\":{\"T\":7}", "200",
        "{\"n\":\"x\",\"ty\":\"T\",\"a\":247,\"w\":1,\"c\":65535,\"io\":{"
        "\"x\":{\"r\":65535,\"t\":\"u32\",\"f\":[6],\"b\":3,\"l\":8,\"e\":\"l\"},"
        "\"y\":{\"r\":0,\"t\":\"u16\",\"f\":[16,4],\"e\":\"b\"}}}"));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_TRUE(cfg->gps_enabled);
    TEST_ASSERT_EQUAL_UINT8(200, cfg->units[0].key);
    TEST_ASSERT_EQUAL_UINT8(247, cfg->units[0].address);
    TEST_ASSERT_EQUAL_UINT16(1, cfg->units[0].wifi_interval_s);
    TEST_ASSERT_EQUAL_UINT16(65535, cfg->units[0].nbiot_interval_s);
    const io_config_t *x = find_io(cfg, 200, "x");
    TEST_ASSERT_EQUAL_UINT16(65535, x->read_addr);
    TEST_ASSERT_EQUAL_UINT8(0, x->read_fc);
    TEST_ASSERT_EQUAL_UINT8(6, x->write_fc);
    TEST_ASSERT_EQUAL_UINT8(3, x->start_byte);
    TEST_ASSERT_EQUAL_UINT8(8, x->length);
    TEST_ASSERT_FALSE(x->big_endian);
    const io_config_t *y = find_io(cfg, 200, "y");
    TEST_ASSERT_EQUAL_UINT8(4, y->read_fc);
    TEST_ASSERT_EQUAL_UINT8(16, y->write_fc);
    TEST_ASSERT_TRUE(y->big_endian);
    TEST_ASSERT_EQUAL_UINT16(7, cfg->configure_units[0].address_register);
    config_manager_release();
}

TEST(parse, unknown_keys_rejected)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with(",\"x\":1", "1", UNIT_OK));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with(",\"MQTT_URL\":\"m\"", "1", UNIT_OK));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with("", "1", "{\"n\":\"x\",\"a\":5,\"io\":{},\"unit_manufacturer\":\"M\"}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_io("{\"r\":1,\"t\":\"u16\",\"f\":[3],\"IO_unit\":\"V\"}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_io("{\"r\":1,\"t\":\"u16\",\"f\":[3],\"min\":0}"));
    // Keys are case-sensitive
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_io("{\"R\":1,\"t\":\"u16\",\"f\":[3]}"));
    // Duplicate keys
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_io("{\"r\":1,\"r\":2,\"t\":\"u16\",\"f\":[3]}"));
    TEST_ASSERT_NULL(config_manager_get_ptr());
}

TEST(parse, v1_file_rejected)
{
    load_example();
    char *before = read_text_file(CONFIG_MANAGER_FILE_PATH);
    char *v1 = read_text_file(SCHEMA_DIR "/../v1/config_example.json");
    TEST_ASSERT_NOT_NULL(v1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save(v1));
    char *after = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_EQUAL(1, config_manager_get_version());
    free(before);
    free(after);

    // REVIEW: a v1 file at the v2 path is not parsed; the library runs on the default (v0) with the v1 device_ID.
    write_text_file(CONFIG_MANAGER_FILE_PATH, v1);
    config_manager_unload();
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(0, cfg->version);
    TEST_ASSERT_EQUAL(0, cfg->unit_count);
    TEST_ASSERT_EQUAL_STRING("8574655FD2B55DF0FFBDC496B91CEF5F", cfg->device_id);
    config_manager_release();
    after = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING(v1, after);   // the bad file is kept
    free(after);
    free(v1);
}

TEST(parse, function_codes)
{
    TEST_ASSERT_EQUAL(ESP_OK, save_io("{\"r\":1,\"t\":\"u16\",\"f\":[3]}"));
    TEST_ASSERT_EQUAL(ESP_OK, save_io("{\"r\":1,\"t\":\"u16\",\"f\":[5]}"));
    TEST_ASSERT_EQUAL(ESP_OK, save_io("{\"r\":1,\"t\":\"u16\",\"f\":[1,5]}"));
    TEST_ASSERT_EQUAL(ESP_OK, save_io("{\"r\":1,\"t\":\"u16\",\"f\":[15,2]}"));

    static const char *const bad[] = {
        "{\"r\":1,\"t\":\"u16\"}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":3}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[\"3\"]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[7]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[0]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3,4]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[5,6]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3,3]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3,6,16]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3.5]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":null}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, save_io(bad[i]), bad[i]);
}

TEST(parse, invalid_values_rejected)
{
    static const char *const bad_io[] = {
        "{\"t\":\"u16\",\"f\":[3]}",
        "{\"r\":65536,\"t\":\"u16\",\"f\":[3]}",
        "{\"r\":-1,\"t\":\"u16\",\"f\":[3]}",
        "{\"r\":1,\"f\":[3]}",
        "{\"r\":1,\"t\":\"FP32\",\"f\":[3]}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3],\"l\":0}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3],\"l\":9}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3],\"b\":256}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3],\"e\":\"little\"}",
        "{\"r\":1,\"t\":\"u16\",\"f\":[3],\"e\":1}",
        "5",
    };
    for (size_t i = 0; i < sizeof(bad_io) / sizeof(bad_io[0]); i++)
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, save_io(bad_io[i]), bad_io[i]);

    // IO name: 31 bytes is the limit
    TEST_ASSERT_EQUAL(ESP_OK, save_with("", "1",
        "{\"n\":\"x\",\"a\":5,\"io\":{\"abcdefghijklmnopqrstuvwxyz01234\":{\"r\":1,\"t\":\"u16\",\"f\":[3]}}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with("", "1",
        "{\"n\":\"x\",\"a\":5,\"io\":{\"abcdefghijklmnopqrstuvwxyz012345\":{\"r\":1,\"t\":\"u16\",\"f\":[3]}}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with("", "1",
        "{\"n\":\"x\",\"a\":5,\"io\":{\"\":{\"r\":1,\"t\":\"u16\",\"f\":[3]}}}"));

    // Unit keys "1"-"255" only
    TEST_ASSERT_EQUAL(ESP_OK, save_with("", "255", UNIT_OK));
    static const char *const bad_keys[] = {"0", "256", "01", "unit_1", "", "1a", "-1", "1000"};
    for (size_t i = 0; i < sizeof(bad_keys) / sizeof(bad_keys[0]); i++)
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, save_with("", bad_keys[i], UNIT_OK), bad_keys[i]);

    // Unit fields
    static const char *const bad_units[] = {
        "{\"a\":5,\"io\":{}}",
        "{\"n\":1,\"a\":5,\"io\":{}}",
        "{\"n\":\"x\",\"io\":{}}",
        "{\"n\":\"x\",\"a\":1,\"io\":{}}",
        "{\"n\":\"x\",\"a\":248,\"io\":{}}",
        "{\"n\":\"x\",\"a\":5}",
        "{\"n\":\"x\",\"a\":5,\"io\":[]}",
        "{\"n\":\"x\",\"a\":5,\"w\":0,\"io\":{}}",
        "{\"n\":\"x\",\"a\":5,\"c\":65536,\"io\":{}}",
        "{\"n\":\"x\",\"a\":5,\"ty\":5,\"io\":{}}",
        "{\"n\":\"x\",\"a\":5.5,\"io\":{}}",
    };
    for (size_t i = 0; i < sizeof(bad_units) / sizeof(bad_units[0]); i++)
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, save_with("", "1", bad_units[i]), bad_units[i]);

    // Root fields
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"s\":1,\"v\":1,\"id\":\"d\",\"n\":\"g\",\"u\":{}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"v\":1,\"id\":\"d\",\"n\":\"g\",\"u\":{}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"s\":2,\"id\":\"d\",\"n\":\"g\",\"u\":{}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"s\":2,\"v\":-1,\"id\":\"d\",\"n\":\"g\",\"u\":{}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"s\":2,\"v\":1,\"n\":\"g\",\"u\":{}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"s\":2,\"v\":1,\"id\":\"d\",\"u\":{}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"g\"}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with(",\"gps\":true", "1", UNIT_OK));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with(",\"gps\":2", "1", UNIT_OK));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with(",\"cu\":{\"T\":70000}", "1", UNIT_OK));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, save_with(",\"cu\":[]", "1", UNIT_OK));
    // Gateway name: 39 bytes is the limit
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(
        "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"123456789012345678901234567890123456789\",\"u\":{}}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save(
        "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"1234567890123456789012345678901234567890\",\"u\":{}}"));
}

TEST(parse, invalid_json_is_rejected)
{
    load_example();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{broken"));
    TEST_ASSERT_EQUAL(3, config_manager_get_ptr()->unit_count);
    config_manager_release();
    config_manager_unload();
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    TEST_ASSERT_EQUAL(3, config_manager_get_ptr()->unit_count);
    config_manager_release();
}

TEST(parse, missing_file_writes_defaults)
{
    write_text_file(CONFIG_MANAGER_V1_FILE_PATH, "{\"config_version\":1}");
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(0, cfg->version);
    TEST_ASSERT_EQUAL_STRING("MIOT-Gateway", cfg->name);
    TEST_ASSERT_EQUAL(0, cfg->unit_count);
    TEST_ASSERT_EQUAL(0, cfg->configure_unit_count);
    TEST_ASSERT_FALSE(cfg->gps_enabled);
    config_manager_release();

    // Writing the factory default does not remove the v1 file (plan section 6)
    char *v1 = read_text_file(CONFIG_MANAGER_V1_FILE_PATH);
    TEST_ASSERT_NOT_NULL(v1);
    free(v1);

    TEST_ASSERT_EQUAL(ESP_OK, config_manager_reset_to_default("GW-TEST-1"));
    TEST_ASSERT_EQUAL_STRING("GW-TEST-1", config_manager_get_string("device_ID"));
    TEST_ASSERT_EQUAL(0, config_manager_get_version());

    // Hand the generated defaults to schema_check.py
    char *json = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_STRING("{\"s\":2,\"v\":0,\"id\":\"GW-TEST-1\",\"n\":\"MIOT-Gateway\",\"u\":{}}", json);
    write_text_file(DEFAULTS_OUT, json);
    free(json);
}

TEST(parse, unload_and_get_ptr)
{
    load_example();
    config_manager_unload();
    TEST_ASSERT_NULL(config_manager_get_ptr());
    TEST_ASSERT_EQUAL(0, config_manager_get_version());
    TEST_ASSERT_EQUAL(-1, config_manager_find_unit(1));
    TEST_ASSERT_NULL(config_manager_build_state(false));
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    TEST_ASSERT_EQUAL(3, config_manager_get_ptr()->unit_count);
    config_manager_release();
}

TEST(parse, find_unit)
{
    load_example();
    TEST_ASSERT_EQUAL(0, config_manager_find_unit(1));
    TEST_ASSERT_EQUAL(2, config_manager_find_unit(3));
    TEST_ASSERT_EQUAL(-1, config_manager_find_unit(4));
    TEST_ASSERT_EQUAL(-1, config_manager_find_unit(0));

    // units[] is sorted by number, not by key string
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(
        "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"g\",\"u\":{\"10\":" UNIT_OK ",\"9\":" UNIT_OK ",\"100\":" UNIT_OK "}}"));
    TEST_ASSERT_EQUAL(0, config_manager_find_unit(9));
    TEST_ASSERT_EQUAL(1, config_manager_find_unit(10));
    TEST_ASSERT_EQUAL(2, config_manager_find_unit(100));
}

TEST(parse, quota_on_save)
{
    char *json = malloc(8192);
    int n = snprintf(json, 8192, "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"g\",\"u\":{");
    for (int i = 1; i <= MAX_UNITS + 1; i++)
        n += snprintf(json + n, 8192 - n, "%s\"%d\":" UNIT_OK, i > 1 ? "," : "", i);
    snprintf(json + n, 8192 - n, "}}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, config_manager_save(json));
    // Exactly MAX_UNITS is fine
    n = snprintf(json, 8192, "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"g\",\"u\":{");
    for (int i = 1; i <= MAX_UNITS; i++)
        n += snprintf(json + n, 8192 - n, "%s\"%d\":" UNIT_OK, i > 1 ? "," : "", i);
    snprintf(json + n, 8192 - n, "}}");
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(json));
    free(json);
}

TEST_GROUP_RUNNER(parse)
{
    RUN_TEST_CASE(parse, example_config);
    RUN_TEST_CASE(parse, every_default);
    RUN_TEST_CASE(parse, explicit_values);
    RUN_TEST_CASE(parse, unknown_keys_rejected);
    RUN_TEST_CASE(parse, v1_file_rejected);
    RUN_TEST_CASE(parse, function_codes);
    RUN_TEST_CASE(parse, invalid_values_rejected);
    RUN_TEST_CASE(parse, invalid_json_is_rejected);
    RUN_TEST_CASE(parse, missing_file_writes_defaults);
    RUN_TEST_CASE(parse, unload_and_get_ptr);
    RUN_TEST_CASE(parse, find_unit);
    RUN_TEST_CASE(parse, quota_on_save);
}
