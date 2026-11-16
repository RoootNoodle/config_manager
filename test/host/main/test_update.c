#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_fixture.h"
#include "test_helpers.h"

TEST_GROUP(update);

TEST_SETUP(update)
{
    test_env_setup();
    load_example();
}

TEST_TEAR_DOWN(update) { test_env_teardown(); }

TEST(update, stale_version_is_skipped)
{
    TEST_ASSERT_FALSE(config_manager_handle_partial_update("{\"config_version\":1,\"name\":\"X\"}"));
    TEST_ASSERT_EQUAL_STRING("MIOT-Gateway", config_manager_get_string("name"));
    TEST_ASSERT_FALSE(config_manager_handle_partial_update("not json"));
}

TEST(update, merge_add_update_delete)
{
    TEST_ASSERT_TRUE(config_manager_handle_partial_update(
        "{\"config_version\":2,\"name\":\"Site A\",\"delete_units\":[\"unit_1\"],"
        "\"configure_units\":{\"MIOT_Trigger\":299},"
        "\"units\":{"
        "\"unit_2\":{\"unit_address\":21,\"IO_config\":{\"relay\":{\"IO_name\":\"relay\",\"IO_function_code\":[3,16]}}},"
        "\"unit_4\":{\"unit_address\":40,\"IO_config\":{\"flow\":{\"IO_name\":\"flow\",\"IO_function_code\":[4]}}}}}"));

    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(2, cfg->version);
    TEST_ASSERT_EQUAL_STRING("Site A", cfg->name);
    TEST_ASSERT_EQUAL(3, cfg->unit_count);
    TEST_ASSERT_EQUAL_STRING("unit_2", cfg->units[0].name);
    TEST_ASSERT_EQUAL_UINT8(21, cfg->units[0].address);
    TEST_ASSERT_EQUAL(1, cfg->units[0].io_count);
    TEST_ASSERT_EQUAL_UINT8(16, find_io(cfg, "unit_2", "relay")->write_fc);
    TEST_ASSERT_EQUAL_UINT8(4, find_io(cfg, "unit_4", "flow")->read_fc);
    TEST_ASSERT_NOT_NULL(find_io(cfg, "unit_3", "temperature"));
    TEST_ASSERT_EQUAL(1, cfg->configure_unit_count);
    char hash_before[MAX_HASH_LEN];
    strcpy(hash_before, cfg->system_hash);
    config_manager_release();

    // The merged config was persisted: a reload gives the same result
    config_manager_unload();
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(2, cfg->version);
    TEST_ASSERT_EQUAL(3, cfg->unit_count);
    TEST_ASSERT_EQUAL_STRING(hash_before, cfg->system_hash);
    config_manager_release();

    // Fields the update did not mention are kept
    cJSON *root = config_manager_get_config_json();
    TEST_ASSERT_EQUAL_STRING("mqtts://your-server.com:8883", cJSON_GetStringValue(cJSON_GetObjectItem(root, "MQTT_URL")));
    cJSON_Delete(root);
}

TEST(update, delete_unit)
{
    TEST_ASSERT_TRUE(config_manager_delete_unit("unit_1"));
    TEST_ASSERT_FALSE(config_manager_delete_unit("unit_1"));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(2, cfg->unit_count);
    config_manager_release();
}

TEST(update, units_payload_and_unit_json)
{
    char *payload = config_manager_get_units_payload("{\"units\":[\"unit_3\",\"nope\"],\"configure_units\":true}");
    TEST_ASSERT_NOT_NULL(payload);
    cJSON *p = cJSON_Parse(payload);
    free(payload);
    TEST_ASSERT_EQUAL(1, cJSON_GetObjectItem(p, "config_version")->valueint);
    cJSON *units = cJSON_GetObjectItem(p, "units");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(units));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(units, "unit_3"));
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(cJSON_GetObjectItem(p, "configure_units")));
    cJSON_Delete(p);

    TEST_ASSERT_NULL(config_manager_get_units_payload("{\"units\":\"unit_3\"}"));

    cJSON *u = config_manager_get_unit_json("unit_1");
    TEST_ASSERT_EQUAL_STRING("Level Sensor", cJSON_GetStringValue(cJSON_GetObjectItem(u, "unit_name")));
    cJSON_Delete(u);
    TEST_ASSERT_NULL(config_manager_get_unit_json("nope"));
}

TEST(update, live_values)
{
    TEST_ASSERT_TRUE(isnan(config_get_live_value("voltage", 1)));
    TEST_ASSERT_EQUAL(ESP_OK, config_update_live_value("voltage", 1, 231.5f));
    TEST_ASSERT_EQUAL_FLOAT(231.5f, config_get_live_value("voltage", 1));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, config_update_live_value("nope", 1, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_update_live_value("voltage", 9, 1));
    TEST_ASSERT_TRUE(isnan(config_get_live_value("voltage", 9)));
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_await_idle(10));
}

TEST_GROUP_RUNNER(update)
{
    RUN_TEST_CASE(update, stale_version_is_skipped);
    RUN_TEST_CASE(update, merge_add_update_delete);
    RUN_TEST_CASE(update, delete_unit);
    RUN_TEST_CASE(update, units_payload_and_unit_json);
    RUN_TEST_CASE(update, live_values);
}
