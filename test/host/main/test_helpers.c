#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "unity.h"
#include "config_manager_priv.h"

void test_env_setup(void)
{
    mkdir(TEST_DIR, 0755);
    remove(CONFIG_MANAGER_FILE_PATH);
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_init());
}

void test_env_teardown(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_deinit());
}

char *read_text_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    size_t n = fread(buf, 1, size, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

void load_example(void)
{
    char *json = read_text_file(EXAMPLE_JSON);
    TEST_ASSERT_NOT_NULL_MESSAGE(json, EXAMPLE_JSON);
    TEST_ASSERT_EQUAL(ESP_OK, config_manager_save(json));
    free(json);
}

const io_config_t *find_io(const system_config_t *cfg, const char *unit, const char *io)
{
    for (int u = 0; u < cfg->unit_count; u++) {
        if (strcmp(cfg->units[u].name, unit) != 0) continue;
        for (int i = 0; i < cfg->units[u].io_count; i++) {
            const io_config_t *p = &cfg->io_pool[cfg->units[u].io_pool_offset + i];
            if (strcmp(p->name, io) == 0) return p;
        }
    }
    return NULL;
}
