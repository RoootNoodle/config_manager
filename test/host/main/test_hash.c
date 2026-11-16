#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "unity_fixture.h"
#include "test_helpers.h"

// Independent reference over schemas/v1/config_example.json:
// sha256(json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False))
#define H_UNIT_1  "309caa4a01ceb4a39de84bb7b870ed40f0b22c1de2ec7daa35a15faab29b8a6c"
#define H_UNIT_2  "d15d6ad323dce36f0ddf7ce6e677852ce643f22a4a26ea910e414c90d08ac3fe"
#define H_UNIT_3  "afef79c61d452e5d4353d6b552097e54d0ae9617d2c5220e8a265dfd5cff5120"
#define H_CU      "e6715cf0d7228d4fc5d521f7484b0e7911ce0d72671040d8c35b698943bcbdef"
#define H_SYSTEM  "5d52554a9400927c81ea7709273e808474894e4e3b5a9bc1dffade77975cad55"

#define SYNC_PREFIX "{\"config_version\":1,\"configure_units_hash\":\"" H_CU "\",\"units_hash\":"

TEST_GROUP(hash);

TEST_SETUP(hash)
{
    test_env_setup();
    load_example();
}

TEST_TEAR_DOWN(hash) { test_env_teardown(); }

TEST(hash, matches_canonical_form)
{
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL_STRING(H_SYSTEM, cfg->system_hash);
    TEST_ASSERT_EQUAL_STRING(H_CU, cfg->configure_units_hash);
    config_manager_release();

    cJSON *hashes = config_manager_get_all_unit_hashes();
    TEST_ASSERT_NOT_NULL(hashes);
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(hashes));
    TEST_ASSERT_EQUAL_STRING(H_UNIT_1, cJSON_GetStringValue(cJSON_GetObjectItem(hashes, "unit_1")));
    TEST_ASSERT_EQUAL_STRING(H_UNIT_2, cJSON_GetStringValue(cJSON_GetObjectItem(hashes, "unit_2")));
    TEST_ASSERT_EQUAL_STRING(H_UNIT_3, cJSON_GetStringValue(cJSON_GetObjectItem(hashes, "unit_3")));
    cJSON_Delete(hashes);

    char cu[MAX_HASH_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_compute_configure_units_hash(cu));
    TEST_ASSERT_EQUAL_STRING(H_CU, cu);
}

TEST(hash, independent_of_key_order)
{
    // Same unit_3 as the example with keys in a different order
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(
        "{\"units\":{\"unit_3\":{\"IO_config\":{\"temperature\":{\"max\":85,\"min\":-40,\"accuracy\":1,"
        "\"IO_endian\":\"big\",\"IO_value_type\":\"INT16\",\"IO_length\":2,\"IO_start_byte\":0,"
        "\"IO_read_address\":300,\"IO_unit\":\"\xC2\xB0" "C\",\"IO_name\":\"temperature\"}},"
        "\"unit_type\":\"MIOT_Temperature_Sensor\",\"unit_manufacturer\":\"MIOT\",\"unit_NB-IOT_interval\":900,"
        "\"unit_wifi_interval\":30,\"unit_default_address\":30,\"unit_address\":30,"
        "\"unit_name\":\"Temperature Sensor\"}},\"config_version\":1}"));
    cJSON *hashes = config_manager_get_all_unit_hashes();
    TEST_ASSERT_EQUAL_STRING(H_UNIT_3, cJSON_GetStringValue(cJSON_GetObjectItem(hashes, "unit_3")));
    cJSON_Delete(hashes);
}

TEST(hash, sync_in_sync)
{
    TEST_ASSERT_FALSE(config_manager_handle_sync(
        SYNC_PREFIX "{\"unit_1\":\"" H_UNIT_1 "\",\"unit_2\":\"" H_UNIT_2 "\",\"unit_3\":\"" H_UNIT_3 "\"}}"));
}

TEST(hash, sync_out_of_sync)
{
    TEST_ASSERT_TRUE(config_manager_handle_sync("{\"config_version\":2}"));
    TEST_ASSERT_TRUE(config_manager_handle_sync(
        SYNC_PREFIX "{\"unit_1\":\"x\",\"unit_2\":\"" H_UNIT_2 "\",\"unit_3\":\"" H_UNIT_3 "\"}}"));
    TEST_ASSERT_TRUE(config_manager_handle_sync(
        SYNC_PREFIX "{\"unit_1\":\"" H_UNIT_1 "\",\"unit_2\":\"" H_UNIT_2 "\"}}"));
    TEST_ASSERT_TRUE(config_manager_handle_sync(
        SYNC_PREFIX "{\"unit_1\":\"" H_UNIT_1 "\",\"unit_2\":\"" H_UNIT_2 "\",\"unit_3\":\"" H_UNIT_3 "\",\"unit_9\":\"y\"}}"));
    TEST_ASSERT_TRUE(config_manager_handle_sync(
        SYNC_PREFIX "{\"unit_1\":42,\"unit_2\":\"" H_UNIT_2 "\",\"unit_3\":\"" H_UNIT_3 "\"}}"));
    TEST_ASSERT_TRUE(config_manager_handle_sync("{\"configure_units_hash\":\"zzz\"}"));
    TEST_ASSERT_FALSE(config_manager_handle_sync("nope"));
}

TEST(hash, sync_delete_units)
{
    TEST_ASSERT_TRUE(config_manager_handle_sync("{\"delete_units\":[\"unit_2\",\"unit_missing\"]}"));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(2, cfg->unit_count);
    TEST_ASSERT_EQUAL_STRING("unit_3", cfg->units[1].name);
    config_manager_release();
}

TEST_GROUP_RUNNER(hash)
{
    RUN_TEST_CASE(hash, matches_canonical_form);
    RUN_TEST_CASE(hash, independent_of_key_order);
    RUN_TEST_CASE(hash, sync_in_sync);
    RUN_TEST_CASE(hash, sync_out_of_sync);
    RUN_TEST_CASE(hash, sync_delete_units);
}
