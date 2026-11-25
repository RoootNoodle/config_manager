#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "unity_fixture.h"
#include "test_helpers.h"

#define H_ROOT    "eee9ed4c"
#define H_UNIT_1  "e9193bf6"
#define H_UNIT_2  "a9f85125"
#define H_UNIT_3  "c1ac7114"

// Escaping, UTF-8, gps 1, unit key "10" before "2", empty io, all write codes; computed the same way.
#define C2_JSON "{\"s\":2,\"v\":1,\"id\":\"ID-\xC3\xA9\",\"n\":\"Tank \\\"A\\\" \xC2\xB0" "C\\\\x\",\"gps\":1," \
                "\"cu\":{\"Zeta\":5,\"Alpha\":7},\"u\":{\"10\":{\"n\":\"Ten\\ttab\\u0001\",\"a\":2,\"io\":{" \
                "\"b\":{\"r\":2,\"t\":\"bool\",\"f\":[1,5]},\"a\":{\"r\":1,\"t\":\"i32\",\"f\":[15],\"b\":2,\"l\":8}}}," \
                "\"2\":{\"n\":\"Two\",\"a\":3,\"io\":{}}}}"
#define C2_ROOT    "02ca0ec3"
#define C2_UNIT_10 "73357223"
#define C2_UNIT_2  "5dcf6d80"

#define C3_JSON "{\"s\":2,\"v\":1,\"id\":\"d\",\"n\":\"g\",\"u\":{\"11\":{\"n\":\"Meter\",\"a\":11,\"io\":{" \
                "\"total\":{\"r\":13,\"t\":\"f32\",\"f\":[4],\"e\":\"ws\"}," \
                "\"total64\":{\"r\":4,\"t\":\"f64\",\"f\":[4,16],\"e\":\"ws\"}," \
                "\"status\":{\"r\":8,\"t\":\"u32\",\"f\":[3],\"e\":\"bs\"}}}}}"
#define C3_ROOT    "5a3f729e"
#define C3_UNIT_11 "fa329295"

TEST_GROUP(hash);

TEST_SETUP(hash)
{
    test_env_setup();
    load_example();
}

TEST_TEAR_DOWN(hash) { test_env_teardown(); }

static void assert_hex(const char *expected, uint32_t hash)
{
    char hex[9];
    snprintf(hex, sizeof(hex), "%08x", (unsigned)hash);
    TEST_ASSERT_EQUAL_STRING(expected, hex);
}

TEST(hash, matches_python_reference)
{
    const system_config_t *cfg = config_manager_get_ptr();
    assert_hex(H_ROOT, cfg->hash);
    assert_hex(H_UNIT_1, cfg->units[0].hash);
    assert_hex(H_UNIT_2, cfg->units[1].hash);
    assert_hex(H_UNIT_3, cfg->units[2].hash);
    config_manager_release();

    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(C2_JSON));
    cfg = config_manager_get_ptr();
    assert_hex(C2_ROOT, cfg->hash);
    TEST_ASSERT_EQUAL_UINT8(2, cfg->units[0].key);
    assert_hex(C2_UNIT_2, cfg->units[0].hash);
    assert_hex(C2_UNIT_10, cfg->units[1].hash);
    config_manager_release();

    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(C3_JSON));
    cfg = config_manager_get_ptr();
    assert_hex(C3_ROOT, cfg->hash);
    assert_hex(C3_UNIT_11, cfg->units[0].hash);
    config_manager_release();
}

TEST(hash, default_written_out_equals_left_out)
{
    // The example with every default written out, keys in another order and f unsorted
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(
        "{\"u\":{"
        "\"3\":{\"io\":{\"temperature\":{\"e\":\"b\",\"l\":2,\"b\":0,\"f\":[3],\"t\":\"i16\",\"r\":300}},"
        "\"c\":900,\"w\":30,\"a\":30,\"ty\":\"MIOT_Temperature_Sensor\",\"n\":\"Temperature Sensor\"},"
        "\"2\":{\"n\":\"Power Meter\",\"ty\":\"MIOT_Power_Sensor\",\"a\":20,\"w\":10,\"c\":600,\"io\":{"
        "\"relay\":{\"r\":210,\"t\":\"u16\",\"f\":[6,3],\"b\":0,\"l\":2,\"e\":\"b\"},"
        "\"voltage\":{\"r\":200,\"t\":\"f32\",\"f\":[4],\"l\":4},"
        "\"current\":{\"r\":202,\"t\":\"f32\",\"f\":[4],\"e\":\"b\"}}},"
        "\"1\":{\"n\":\"Level Sensor\",\"ty\":\"MIOT_Level_Sensor\",\"a\":10,\"w\":5,\"c\":300,"
        "\"io\":{\"level\":{\"r\":100,\"t\":\"f32\",\"f\":[3],\"b\":0,\"l\":4,\"e\":\"b\"}}}},"
        "\"cu\":{\"MIOT_Temperature_Sensor\":303,\"MIOT_Power_Sensor\":299,\"MIOT_Level_Sensor\":298},"
        "\"gps\":0,\"n\":\"MIOT-Gateway\",\"id\":\"8574655FD2B55DF0FFBDC496B91CEF5F\",\"v\":7,\"s\":2}"));
    const system_config_t *cfg = config_manager_get_ptr();
    assert_hex(H_ROOT, cfg->hash);   // v is not part of the root hash
    assert_hex(H_UNIT_1, cfg->units[0].hash);
    assert_hex(H_UNIT_2, cfg->units[1].hash);
    assert_hex(H_UNIT_3, cfg->units[2].hash);
    config_manager_release();
}

TEST(hash, sync)
{
    TEST_ASSERT_FALSE(config_manager_handle_sync("{\"v\":1,\"h\":\"" H_ROOT "\"}"));
    // Only h is compared
    TEST_ASSERT_FALSE(config_manager_handle_sync("{\"v\":5,\"h\":\"" H_ROOT "\"}"));
    TEST_ASSERT_TRUE(config_manager_handle_sync("{\"v\":1,\"h\":\"00000000\"}"));
    TEST_ASSERT_TRUE(config_manager_handle_sync("{\"v\":1,\"h\":\"EEE9ED4C\"}"));
    // Malformed: ignored
    TEST_ASSERT_FALSE(config_manager_handle_sync("{\"v\":1}"));
    TEST_ASSERT_FALSE(config_manager_handle_sync("{\"v\":1,\"h\":5}"));
    TEST_ASSERT_FALSE(config_manager_handle_sync("nope"));
    TEST_ASSERT_FALSE(config_manager_handle_sync(NULL));
}

TEST(hash, state_on_connect_and_after_sync_mismatch)
{
    cJSON *state = config_manager_build_state(false);
    TEST_ASSERT_NOT_NULL(state);
    char *s = cJSON_PrintUnformatted(state);
    TEST_ASSERT_EQUAL_STRING("{\"s\":2,\"v\":1,\"h\":\"" H_ROOT "\"}", s);
    cJSON_free(s);
    cJSON_Delete(state);

    TEST_ASSERT_TRUE(config_manager_handle_sync("{\"v\":1,\"h\":\"12345678\"}"));
    state = config_manager_build_state(true);
    s = cJSON_PrintUnformatted(state);
    TEST_ASSERT_EQUAL_STRING("{\"s\":2,\"v\":1,\"h\":\"" H_ROOT "\",\"u\":{\"1\":\"" H_UNIT_1 "\",\"2\":\""
                             H_UNIT_2 "\",\"3\":\"" H_UNIT_3 "\"}}", s);
    cJSON_free(s);
    cJSON_Delete(state);
}

TEST_GROUP_RUNNER(hash)
{
    RUN_TEST_CASE(hash, matches_python_reference);
    RUN_TEST_CASE(hash, default_written_out_equals_left_out);
    RUN_TEST_CASE(hash, sync);
    RUN_TEST_CASE(hash, state_on_connect_and_after_sync_mismatch);
}
