#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_fixture.h"
#include "test_helpers.h"
#include "config_manager_priv.h"

// REVIEW: expected hashes after each patch come from test/hash_reference.py with an RFC 7396 merge in Python.
#define H_ROOT_0 "eee9ed4c"
#define H_UNIT_3 "c1ac7114"

TEST_GROUP(update);

TEST_SETUP(update)
{
    test_env_setup();
    load_example();
}

TEST_TEAR_DOWN(update) { test_env_teardown(); }

// Applies a patch, checks its result, and returns the state that follows (caller cJSON_Delete()).
static cJSON *patch(const char *body, esp_err_t expected)
{
    TEST_ASSERT_EQUAL_MESSAGE(expected, config_manager_apply_patch(body), body);
    cJSON *state = config_manager_build_state(false);
    TEST_ASSERT_NOT_NULL(state);
    return state;
}

static void assert_state(const char *expected_json, cJSON *state)
{
    char *s = cJSON_PrintUnformatted(state);
    TEST_ASSERT_EQUAL_STRING(expected_json, s);
    cJSON_free(s);
    cJSON_Delete(state);
}

TEST(update, patch_sequence)
{
    assert_state("{\"s\":2,\"v\":2,\"h\":\"ec2fd0c1\",\"u\":{\"2\":\"57c9a537\"}}",
                 patch("{\"v\":2,\"u\":{\"2\":{\"c\":900}}}", ESP_OK));
    // The state reports a patch once; the next one is the plain connect state
    assert_state("{\"s\":2,\"v\":2,\"h\":\"ec2fd0c1\"}", config_manager_build_state(false));

    // No unit changed: "u" is left out, same as the connect state
    assert_state("{\"s\":2,\"v\":3,\"h\":\"61b9ad1b\"}",
                 patch("{\"v\":3,\"n\":\"Pump house\"}", ESP_OK));
    TEST_ASSERT_EQUAL_STRING("Pump house", config_manager_get_string("name"));

    assert_state("{\"s\":2,\"v\":4,\"h\":\"1d70113f\",\"u\":{\"2\":\"98612570\"}}",
                 patch("{\"v\":4,\"u\":{\"2\":{\"io\":{\"power\":{\"r\":204,\"t\":\"f32\",\"f\":[4]}}}}}", ESP_OK));

    assert_state("{\"s\":2,\"v\":5,\"h\":\"aaae84d1\",\"u\":{\"1\":\"4f439e5e\"}}",
                 patch("{\"v\":5,\"u\":{\"1\":{\"io\":{\"level\":{\"f\":[16,3]}}}}}", ESP_OK));
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL_UINT8(16, find_io(cfg, 1, "level")->write_fc);
    config_manager_release();

    assert_state("{\"s\":2,\"v\":6,\"h\":\"0d3aaf68\",\"u\":{\"2\":\"3439f1f5\"}}",
                 patch("{\"v\":6,\"u\":{\"2\":{\"io\":{\"relay\":null}}}}", ESP_OK));

    assert_state("{\"s\":2,\"v\":7,\"h\":\"8965b9d1\",\"u\":{\"3\":null}}",
                 patch("{\"v\":7,\"u\":{\"3\":null}}", ESP_OK));

    assert_state("{\"s\":2,\"v\":8,\"h\":\"92004fc0\",\"u\":{\"4\":\"69b5eded\"}}",
                 patch("{\"v\":8,\"u\":{\"4\":{\"n\":\"Flow\",\"a\":40,\"io\":{\"flow\":{\"r\":1,\"t\":\"u32\",\"f\":[4],\"e\":\"l\"}}}}}", ESP_OK));

    cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(8, cfg->version);
    TEST_ASSERT_EQUAL(3, cfg->unit_count);
    TEST_ASSERT_EQUAL_UINT8(4, cfg->units[2].key);
    TEST_ASSERT_NULL(find_io(cfg, 2, "relay"));
    TEST_ASSERT_NOT_NULL(find_io(cfg, 2, "power"));
    TEST_ASSERT_FALSE(find_io(cfg, 4, "flow")->big_endian);
    uint32_t hash = cfg->hash;
    config_manager_release();

    // The merged file was persisted: a reload gives the same result
    config_manager_unload();
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(8, cfg->version);
    TEST_ASSERT_EQUAL_UINT32(hash, cfg->hash);
    config_manager_release();
}

// REVIEW: equal v is a duplicate delivery: ESP_OK, nothing applied, next state has neither u nor err.
TEST(update, duplicate_version)
{
    const char *body = "{\"v\":2,\"u\":{\"2\":{\"c\":900}}}";
    assert_state("{\"s\":2,\"v\":2,\"h\":\"ec2fd0c1\",\"u\":{\"2\":\"57c9a537\"}}", patch(body, ESP_OK));
    TEST_ASSERT_EQUAL(ESP_OK, config_update_live_value("voltage", 1, 5.0f));
    char *before = read_text_file(CONFIG_MANAGER_FILE_PATH);

    // Same patch again (QoS 1 redelivery)
    assert_state("{\"s\":2,\"v\":2,\"h\":\"ec2fd0c1\"}", patch(body, ESP_OK));
    // A different patch reusing v 2 is ignored the same way; Web sees only the unchanged hash
    assert_state("{\"s\":2,\"v\":2,\"h\":\"ec2fd0c1\"}", patch("{\"v\":2,\"n\":\"Other\"}", ESP_OK));
    // Even when it would be invalid
    assert_state("{\"s\":2,\"v\":2,\"h\":\"ec2fd0c1\"}", patch("{\"v\":2,\"x\":1}", ESP_OK));

    char *after = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    TEST_ASSERT_EQUAL_STRING("MIOT-Gateway", config_manager_get_string("name"));
    // The active config was not rebuilt, so live values are untouched
    TEST_ASSERT_EQUAL_FLOAT(5.0f, config_get_live_value("voltage", 1));

    // REVIEW: a duplicate keeps an unreported patch result: redelivery before build_state still reports unit 1.
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_apply_patch("{\"v\":3,\"u\":{\"1\":{\"w\":6}}}"));
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_apply_patch("{\"v\":3,\"u\":{\"1\":{\"w\":6}}}"));
    cJSON *state = config_manager_build_state(false);
    TEST_ASSERT_NOT_NULL(state_unit(state, "1"));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(cJSON_GetObjectItem(state, "u")));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(state, "err"));
    cJSON_Delete(state);
    // Reported once; then the plain state
    state = config_manager_build_state(false);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(state, "u"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(state, "err"));
    cJSON_Delete(state);
}

TEST(update, rejected_patch_leaves_file_intact)
{
    char *before = read_text_file(CONFIG_MANAGER_FILE_PATH);
    static const struct { const char *body; esp_err_t ret; const char *err; } cases[] = {
        // Result is not a valid v2 file: f deleted, a out of range, wrong type, unit without required keys
        {"{\"v\":2,\"u\":{\"1\":{\"io\":{\"level\":{\"f\":null}}}}}", ESP_ERR_INVALID_ARG, "inv"},
        {"{\"v\":2,\"u\":{\"1\":{\"a\":300}}}", ESP_ERR_INVALID_ARG, "inv"},
        {"{\"v\":2,\"u\":{\"1\":5}}", ESP_ERR_INVALID_ARG, "inv"},
        {"{\"v\":2,\"u\":{\"9\":{\"n\":\"x\"}}}", ESP_ERR_INVALID_ARG, "inv"},
        {"{\"v\":2,\"s\":null}", ESP_ERR_INVALID_ARG, "inv"},
        // Unknown keys
        {"{\"v\":2,\"x\":1}", ESP_ERR_INVALID_ARG, "inv"},
        {"{\"v\":2,\"u\":{\"1\":{\"unit_name\":\"x\"}}}", ESP_ERR_INVALID_ARG, "inv"},
        {"{\"v\":2,\"u\":{\"1\":{\"io\":{\"level\":{\"IO_name\":\"x\"}}}}}", ESP_ERR_INVALID_ARG, "inv"},
        {"{\"v\":2,\"u\":{\"unit_1\":null}}", ESP_ERR_INVALID_ARG, "inv"},
        // REVIEW: lower or missing v is rejected with "v"; an equal v is a duplicate (test duplicate_version).
        {"{\"v\":0,\"n\":\"X\"}", ESP_ERR_INVALID_VERSION, "v"},
        {"{\"n\":\"X\"}", ESP_ERR_INVALID_VERSION, "v"},
        {"{\"v\":\"2\",\"n\":\"X\"}", ESP_ERR_INVALID_VERSION, "v"},
        // Not JSON / not an object
        {"not json", ESP_ERR_INVALID_ARG, "inv"},
        {"[1]", ESP_ERR_INVALID_ARG, "inv"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char expected[96];
        snprintf(expected, sizeof(expected), "{\"s\":2,\"v\":1,\"h\":\"" H_ROOT_0 "\",\"err\":\"%s\"}", cases[i].err);
        assert_state(expected, patch(cases[i].body, cases[i].ret));
        char *after = read_text_file(CONFIG_MANAGER_FILE_PATH);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(before, after, cases[i].body);
        free(after);
    }
    free(before);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_apply_patch(NULL));

    // The active config is the old one
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(1, cfg->version);
    TEST_ASSERT_EQUAL(3, cfg->unit_count);
    config_manager_release();
}

TEST(update, quota)
{
    char *before = read_text_file(CONFIG_MANAGER_FILE_PATH);
    char *body = malloc(16384);
    TEST_ASSERT_NOT_NULL(body);

    // Units: the example has 3, add MAX_UNITS - 2 more
    int n = snprintf(body, 16384, "{\"v\":2,\"u\":{");
    for (int i = 0; i < MAX_UNITS - 2; i++)
        n += snprintf(body + n, 16384 - n, "%s\"%d\":{\"n\":\"x\",\"a\":5,\"io\":{}}", i ? "," : "", 10 + i);
    snprintf(body + n, 16384 - n, "}}");
    assert_state("{\"s\":2,\"v\":1,\"h\":\"" H_ROOT_0 "\",\"err\":\"q\"}", patch(body, ESP_ERR_INVALID_SIZE));

    // IOs: the example has 5, add MAX_IOS - 4 more in one unit
    n = snprintf(body, 16384, "{\"v\":2,\"u\":{\"9\":{\"n\":\"x\",\"a\":5,\"io\":{");
    for (int i = 0; i < CONFIG_CONFIG_MANAGER_MAX_IOS - 4; i++)
        n += snprintf(body + n, 16384 - n, "%s\"%d\":{\"r\":1,\"t\":\"u16\",\"f\":[3]}", i ? "," : "", i);
    snprintf(body + n, 16384 - n, "}}}}");
    TEST_ASSERT_LESS_THAN(CONFIG_CONFIG_MANAGER_MAX_FILE_SIZE, n);   // so this is the IO quota, not the size quota
    assert_state("{\"s\":2,\"v\":1,\"h\":\"" H_ROOT_0 "\",\"err\":\"q\"}", patch(body, ESP_ERR_INVALID_SIZE));

    // Size: a unit name long enough to push the file over MAX_FILE_SIZE
    n = snprintf(body, 16384, "{\"v\":2,\"u\":{\"9\":{\"n\":\"");
    memset(body + n, 'x', CONFIG_CONFIG_MANAGER_MAX_FILE_SIZE);
    n += CONFIG_CONFIG_MANAGER_MAX_FILE_SIZE;
    snprintf(body + n, 16384 - n, "\",\"a\":5,\"io\":{}}}}");
    assert_state("{\"s\":2,\"v\":1,\"h\":\"" H_ROOT_0 "\",\"err\":\"q\"}", patch(body, ESP_ERR_INVALID_SIZE));

    char *after = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(after);
    free(before);

    // Up to the limit is accepted
    n = snprintf(body, 16384, "{\"v\":2,\"u\":{\"9\":{\"n\":\"x\",\"a\":5,\"io\":{");
    for (int i = 0; i < CONFIG_CONFIG_MANAGER_MAX_IOS - 5; i++)
        n += snprintf(body + n, 16384 - n, "%s\"%d\":{\"r\":1,\"t\":\"u16\",\"f\":[3]}", i ? "," : "", i);
    snprintf(body + n, 16384 - n, "}}}}");
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_apply_patch(body));
    free(body);

    cJSON *limits = config_manager_build_limits();
    char *s = cJSON_PrintUnformatted(limits);
    char expected[96];
    snprintf(expected, sizeof(expected), "{\"u\":%d,\"io\":%d,\"cu\":%d,\"b\":%d}", MAX_UNITS,
             CONFIG_CONFIG_MANAGER_MAX_IOS, MAX_CONFIGURE_UNITS, CONFIG_CONFIG_MANAGER_MAX_FILE_SIZE);
    TEST_ASSERT_EQUAL_STRING(expected, s);
    cJSON_free(s);
    cJSON_Delete(limits);
}

TEST(update, full_config_as_patch)
{
    // Every key, null for the units to drop
    assert_state("{\"s\":2,\"v\":9,\"h\":\"92004fc0\",\"u\":{\"1\":\"4f439e5e\",\"2\":\"3439f1f5\",\"3\":null,\"4\":\"69b5eded\"}}",
        patch("{\"s\":2,\"v\":9,\"id\":\"8574655FD2B55DF0FFBDC496B91CEF5F\",\"n\":\"Pump house\",\"gps\":0,"
              "\"cu\":{\"MIOT_Level_Sensor\":298,\"MIOT_Power_Sensor\":299,\"MIOT_Temperature_Sensor\":303},"
              "\"u\":{\"1\":{\"n\":\"Level Sensor\",\"ty\":\"MIOT_Level_Sensor\",\"a\":10,\"w\":5,\"c\":300,"
              "\"io\":{\"level\":{\"r\":100,\"t\":\"f32\",\"f\":[3,16]}}},"
              "\"2\":{\"n\":\"Power Meter\",\"ty\":\"MIOT_Power_Sensor\",\"a\":20,\"w\":10,\"c\":900,\"io\":{"
              "\"voltage\":{\"r\":200,\"t\":\"f32\",\"f\":[4]},\"current\":{\"r\":202,\"t\":\"f32\",\"f\":[4]},"
              "\"power\":{\"r\":204,\"t\":\"f32\",\"f\":[4]},\"relay\":null}},"
              "\"3\":null,"
              "\"4\":{\"n\":\"Flow\",\"a\":40,\"io\":{\"flow\":{\"r\":1,\"t\":\"u32\",\"f\":[4],\"e\":\"l\"}}}}}", ESP_OK));
}

TEST(update, live_values_across_patch)
{
    TEST_ASSERT_TRUE(isnan(config_get_live_value("voltage", 1)));
    TEST_ASSERT_EQUAL(ESP_OK, config_update_live_value("voltage", 1, 231.5f));
    TEST_ASSERT_EQUAL_FLOAT(231.5f, config_get_live_value("voltage", 1));
    TEST_ASSERT_EQUAL(ESP_OK, config_update_live_value("temperature", 2, 21.0f));
    TEST_ASSERT_EQUAL(ESP_OK, config_update_live_value("level", 0, 7.0f));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, config_update_live_value("nope", 1, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_update_live_value("voltage", 9, 1));
    TEST_ASSERT_TRUE(isnan(config_get_live_value("voltage", 9)));

    // Unit 2 changes, unit 1 is deleted, unit 3 does not change
    cJSON_Delete(patch("{\"v\":2,\"u\":{\"1\":null,\"2\":{\"c\":900}}}", ESP_OK));
    int idx2 = config_manager_find_unit(2);
    int idx3 = config_manager_find_unit(3);
    TEST_ASSERT_EQUAL(-1, config_manager_find_unit(1));
    TEST_ASSERT_TRUE(isnan(config_get_live_value("voltage", (uint8_t)idx2)));
    TEST_ASSERT_EQUAL_FLOAT(21.0f, config_get_live_value("temperature", (uint8_t)idx3));

    // A rejected patch keeps all live values
    TEST_ASSERT_EQUAL(ESP_OK, config_update_live_value("voltage", (uint8_t)idx2, 230.0f));
    cJSON_Delete(patch("{\"v\":3,\"u\":{\"2\":{\"a\":1}}}", ESP_ERR_INVALID_ARG));
    TEST_ASSERT_EQUAL_FLOAT(230.0f, config_get_live_value("voltage", (uint8_t)idx2));
    TEST_ASSERT_EQUAL_FLOAT(21.0f, config_get_live_value("temperature", (uint8_t)idx3));
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_await_idle(10));
}

TEST(update, get_payload_and_unit_json)
{
    char *payload = config_manager_get_payload("{}");
    char *file = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING(file, payload);
    free(file);
    free(payload);

    payload = config_manager_get_payload("{\"u\":[\"2\",\"9\",3]}");
    TEST_ASSERT_EQUAL_STRING("{\"s\":2,\"v\":1,\"u\":{\"2\":{\"n\":\"Power Meter\",\"ty\":\"MIOT_Power_Sensor\",\"a\":20,"
        "\"w\":10,\"c\":600,\"io\":{\"voltage\":{\"r\":200,\"t\":\"f32\",\"f\":[4]},\"current\":{\"r\":202,\"t\":\"f32\","
        "\"f\":[4]},\"relay\":{\"r\":210,\"t\":\"u16\",\"f\":[3,6]}}}}}", payload);
    free(payload);

    payload = config_manager_get_payload("{\"cu\":1}");
    TEST_ASSERT_EQUAL_STRING("{\"s\":2,\"v\":1,\"cu\":{\"MIOT_Level_Sensor\":298,\"MIOT_Power_Sensor\":299,"
                             "\"MIOT_Temperature_Sensor\":303}}", payload);
    free(payload);

    payload = config_manager_get_payload("{\"u\":[\"3\"],\"cu\":true}");
    cJSON *p = cJSON_Parse(payload);
    free(payload);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(cJSON_GetObjectItem(p, "u"), "3"));
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(cJSON_GetObjectItem(p, "cu")));
    cJSON_Delete(p);

    TEST_ASSERT_NULL(config_manager_get_payload("{\"u\":\"2\"}"));
    TEST_ASSERT_NULL(config_manager_get_payload("nope"));
    TEST_ASSERT_NULL(config_manager_get_payload(NULL));

    cJSON *u = config_manager_get_unit_json(1);
    TEST_ASSERT_EQUAL_STRING("Level Sensor", cJSON_GetStringValue(cJSON_GetObjectItem(u, "n")));
    cJSON_Delete(u);
    TEST_ASSERT_NULL(config_manager_get_unit_json(9));
}

TEST(update, v1_file_removed_after_v2_save)
{
    // By a patch
    write_text_file(CONFIG_MANAGER_V1_FILE_PATH, "{\"config_version\":1}");
    cJSON_Delete(patch("{\"v\":0}", ESP_ERR_INVALID_VERSION));
    char *v1 = read_text_file(CONFIG_MANAGER_V1_FILE_PATH);
    TEST_ASSERT_NOT_NULL_MESSAGE(v1, "a rejected patch must not remove the v1 file");
    free(v1);
    cJSON_Delete(patch("{\"v\":1}", ESP_OK));
    v1 = read_text_file(CONFIG_MANAGER_V1_FILE_PATH);
    TEST_ASSERT_NOT_NULL_MESSAGE(v1, "a duplicate patch must not remove the v1 file");
    free(v1);
    cJSON_Delete(patch("{\"v\":2}", ESP_OK));
    TEST_ASSERT_NULL(read_text_file(CONFIG_MANAGER_V1_FILE_PATH));

    // By a full save
    write_text_file(CONFIG_MANAGER_V1_FILE_PATH, "{\"config_version\":1}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_manager_save("{\"config_version\":3}"));
    v1 = read_text_file(CONFIG_MANAGER_V1_FILE_PATH);
    TEST_ASSERT_NOT_NULL(v1);
    free(v1);
    load_example();
    TEST_ASSERT_NULL(read_text_file(CONFIG_MANAGER_V1_FILE_PATH));
}

// Example file as a full-config patch with the given v (caller free())
static char *example_as_patch(int v)
{
    char *text = read_text_file(EXAMPLE_JSON);
    cJSON *root = cJSON_Parse(text);
    free(text);
    cJSON_ReplaceItemInObjectCaseSensitive(root, "v", cJSON_CreateNumber(v));
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static void assert_fallback(const char *device_id)
{
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(0, cfg->version);
    TEST_ASSERT_EQUAL(0, cfg->unit_count);
    TEST_ASSERT_EQUAL_STRING("MIOT-Gateway", cfg->name);
    TEST_ASSERT_EQUAL_STRING(device_id, cfg->device_id);
    config_manager_release();
    cJSON *state = config_manager_build_state(false);
    TEST_ASSERT_EQUAL(0, cJSON_GetObjectItem(state, "v")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(state, "u"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(state, "err"));
    TEST_ASSERT_TRUE(strcmp(H_ROOT_0, state_str(state, "h")) != 0);   // the server will not match it
    cJSON_Delete(state);
}

// REVIEW: plan section 6 flow for an unreadable file: default v0 in RAM, file kept, full patch accepted and written.
TEST(update, corrupt_file_falls_back_to_default)
{
    const char *bad = "{\"s\":2,\"v\":5,\"id\":\"GW-9\",\"n\":";   // truncated
    write_text_file(CONFIG_MANAGER_FILE_PATH, bad);
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    // Not parseable, so the ID comes from the config that was in RAM
    assert_fallback("8574655FD2B55DF0FFBDC496B91CEF5F");
    char *file = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING(bad, file);
    free(file);

    // A rejected patch keeps the fallback and the bad file
    cJSON_Delete(patch("{\"v\":1,\"u\":{\"1\":{\"n\":\"x\"}}}", ESP_ERR_INVALID_ARG));
    assert_fallback("8574655FD2B55DF0FFBDC496B91CEF5F");
    file = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING(bad, file);
    free(file);

    // The server sends the full config; it is accepted and written
    char *full = example_as_patch(6);
    assert_state("{\"s\":2,\"v\":6,\"h\":\"" H_ROOT_0 "\",\"u\":{\"1\":\"e9193bf6\",\"2\":\"a9f85125\",\"3\":\"" H_UNIT_3 "\"}}",
                 patch(full, ESP_OK));
    free(full);

    // The file is valid again: a reload gives the same config
    config_manager_unload();
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    const system_config_t *cfg = config_manager_get_ptr();
    TEST_ASSERT_EQUAL(6, cfg->version);
    TEST_ASSERT_EQUAL(3, cfg->unit_count);
    config_manager_release();
    cJSON *state = config_manager_build_state(false);
    TEST_ASSERT_EQUAL_STRING(H_ROOT_0, state_str(state, "h"));
    cJSON_Delete(state);
}

TEST(update, invalid_or_empty_file_falls_back_to_default)
{
    // Valid JSON, not a valid v2 file: the ID in it is kept
    config_manager_unload();
    write_text_file(CONFIG_MANAGER_FILE_PATH, "{\"s\":2,\"v\":3,\"id\":\"GW-7\",\"n\":\"g\"}");
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    assert_fallback("GW-7");

    // A partial patch merges onto the default; apply_patch never reports ESP_ERR_INVALID_STATE here
    cJSON_Delete(patch("{\"v\":1,\"n\":\"Site\"}", ESP_OK));
    TEST_ASSERT_EQUAL_STRING("Site", config_manager_get_string("name"));
    config_manager_unload();
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    TEST_ASSERT_EQUAL(1, config_manager_get_version());

    // Empty file, nothing in RAM: no ID known
    config_manager_unload();
    write_text_file(CONFIG_MANAGER_FILE_PATH, "");
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_load());
    assert_fallback("");
    char *file = read_text_file(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL_STRING("", file);   // not overwritten with the default
    free(file);
}

TEST_GROUP_RUNNER(update)
{
    RUN_TEST_CASE(update, patch_sequence);
    RUN_TEST_CASE(update, duplicate_version);
    RUN_TEST_CASE(update, rejected_patch_leaves_file_intact);
    RUN_TEST_CASE(update, quota);
    RUN_TEST_CASE(update, full_config_as_patch);
    RUN_TEST_CASE(update, live_values_across_patch);
    RUN_TEST_CASE(update, get_payload_and_unit_json);
    RUN_TEST_CASE(update, v1_file_removed_after_v2_save);
    RUN_TEST_CASE(update, corrupt_file_falls_back_to_default);
    RUN_TEST_CASE(update, invalid_or_empty_file_falls_back_to_default);
}
