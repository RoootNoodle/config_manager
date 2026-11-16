#include "config_manager.h"
#include "config_manager_priv.h"
#include "cm_alloc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "CONFIG_MGR";

// Protects the config file
static SemaphoreHandle_t s_file_mux   = NULL;
// Protects g_config and io_pool contents (recursive: Modbus re-enters it for live writes)
static SemaphoreHandle_t s_config_mux = NULL;

static system_config_t *g_config = NULL;

static bool write_config_file(const char *json_str);

static size_t free_heap(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void free_config_struct(system_config_t *cfg)
{
    if (!cfg) return;
    cm_free(cfg->io_pool);
    cm_free(cfg->configure_units);
    cm_free(cfg);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) { fclose(f); return NULL; }

    char *buf = cm_malloc(size + 1);
    if (buf) {
        size_t n = fread(buf, 1, size, f);
        buf[n] = '\0';
    }
    fclose(f);
    return buf;
}

// Returns the parsed config file; size_out receives the raw byte count (serialisation size hint).
static cJSON *load_config_file(long *size_out)
{
    if (size_out) *size_out = 0;
    if (xSemaphoreTakeRecursive(s_file_mux, pdMS_TO_TICKS(5000)) != pdTRUE)
        return NULL;
    char *json_str = read_file(CONFIG_MANAGER_FILE_PATH);
    xSemaphoreGiveRecursive(s_file_mux);

    if (!json_str) return NULL;
    if (size_out) *size_out = (long)strlen(json_str);

    cJSON *root = cJSON_Parse(json_str);
    cm_free(json_str);
    return root;
}

// Pre-allocating avoids cJSON's realloc-doubling (8 K -> 16 K needs ~24 K contiguous) on large configs.
// The result is freed with cm_free() whichever path produced it.
static char *serialize_cjson(cJSON *tree, long size_hint)
{
    if (size_hint > 0) {
        int buf_size = (int)size_hint + 1024;
        char *buf = cm_malloc(buf_size);
        if (buf) {
            if (cJSON_PrintPreallocated(tree, buf, buf_size, 0))
                return buf;
            cm_free(buf);
        }
    }
    return cJSON_PrintUnformatted(tree);
}

static io_value_type_t parse_value_type_str(const char *str)
{
    if (!str) return IO_VTYPE_UINT16;
    if (strcmp(str, "FP32")   == 0 || strcmp(str, "FLOAT32") == 0) return IO_VTYPE_FP32;
    if (strcmp(str, "INT32")  == 0) return IO_VTYPE_INT32;
    if (strcmp(str, "UINT32") == 0) return IO_VTYPE_UINT32;
    if (strcmp(str, "INT16")  == 0) return IO_VTYPE_INT16;
    if (strcmp(str, "BOOL")   == 0) return IO_VTYPE_BOOL;
    return IO_VTYPE_UINT16;
}

static bool is_read_fc(int fc)  { return fc >= 1 && fc <= 4; }
static bool is_write_fc(int fc) { return fc == 5 || fc == 6 || fc == 15 || fc == 16; }

// Schema v1: IO_function_code wins; legacy IO_type is used only when it is absent; neither = FC3 + FC6.
static void parse_function_codes(cJSON *io_json, io_config_t *io)
{
    io->read_fc  = 0;
    io->write_fc = 0;

    cJSON *codes = cJSON_GetObjectItem(io_json, "IO_function_code");
    if (cJSON_IsArray(codes)) {
        cJSON *c;
        cJSON_ArrayForEach(c, codes) {
            if (!cJSON_IsNumber(c)) continue;
            if (is_read_fc(c->valueint))       io->read_fc  = (uint8_t)c->valueint;
            else if (is_write_fc(c->valueint)) io->write_fc = (uint8_t)c->valueint;
        }
        if (io->read_fc || io->write_fc) return;
        ESP_LOGW(TAG, "IO '%s': IO_function_code has no valid code, using defaults", io->name);
    } else {
        cJSON *io_type = cJSON_GetObjectItem(io_json, "IO_type");
        if (io_type) {
            int val = 0;
            if (cJSON_IsNumber(io_type)) {
                val = io_type->valueint;
            } else if (cJSON_IsString(io_type)) {
                if      (strcmp(io_type->valuestring, "R") == 0) val = 3;
                else if (strcmp(io_type->valuestring, "W") == 0) val = 6;
                else val = atoi(io_type->valuestring);
            }
            if (is_read_fc(val))   io->read_fc  = (uint8_t)val;
            else if (val >= 5)     io->write_fc = (uint8_t)val;
            return;
        }
    }

    io->read_fc  = 3;
    io->write_fc = 6;
}

static void parse_io(cJSON *io_json, io_config_t *io)
{
    cJSON *name_item = cJSON_GetObjectItem(io_json, "IO_name");
    const char *name_str = (name_item && name_item->valuestring) ? name_item->valuestring : io_json->string;
    strncpy(io->name, name_str ? name_str : "unknown", MAX_NAME_LEN - 1);

    cJSON *tmp;
    io->read_addr  = (tmp = cJSON_GetObjectItem(io_json, "IO_read_address")) ? (uint16_t)tmp->valueint : 0;
    io->start_byte = (tmp = cJSON_GetObjectItem(io_json, "IO_start_byte"))   ? (uint8_t)tmp->valueint  : 0;
    io->length     = (tmp = cJSON_GetObjectItem(io_json, "IO_length"))       ? (uint8_t)tmp->valueint  : 2;

    cJSON *vtype   = cJSON_GetObjectItem(io_json, "IO_value_type");
    io->value_type = parse_value_type_str(vtype ? vtype->valuestring : NULL);

    cJSON *endian  = cJSON_GetObjectItem(io_json, "IO_endian");
    io->big_endian = !(endian && endian->valuestring && strcmp(endian->valuestring, "little") == 0);

    parse_function_codes(io_json, io);
    io->live_value = NAN;
}

static void bytes_to_hex(const unsigned char *bytes, size_t len, char *hex_out)
{
    const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex_out[i * 2]     = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex_out[len * 2] = '\0';
}

// Must stay byte-identical to the server's canonical form (keys sorted by strcmp at every level).
static cJSON *sort_json_keys_recursive(cJSON *item)
{
    if (!item) return NULL;
    if (cJSON_IsObject(item)) {
        int count = 0;
        for (cJSON *c = item->child; c; c = c->next) count++;
        if (count <= 1) {
            for (cJSON *c = item->child; c; c = c->next) sort_json_keys_recursive(c);
            return item;
        }
        cJSON **children = cm_malloc(count * sizeof(cJSON *));
        if (!children) return item;
        int i = 0;
        for (cJSON *c = item->child; c; c = c->next) children[i++] = c;
        for (int a = 0; a < count - 1; a++)
            for (int b = 0; b < count - a - 1; b++)
                if (strcmp(children[b]->string, children[b + 1]->string) > 0) {
                    cJSON *t = children[b]; children[b] = children[b + 1]; children[b + 1] = t;
                }
        item->child = children[0];
        for (int j = 0; j < count - 1; j++) {
            children[j]->next     = children[j + 1];
            children[j + 1]->prev = children[j];
        }
        children[0]->prev         = children[count - 1];
        children[count - 1]->next = NULL;
        cm_free(children);
        for (cJSON *c = item->child; c; c = c->next) sort_json_keys_recursive(c);
    } else if (cJSON_IsArray(item)) {
        for (cJSON *c = item->child; c; c = c->next) sort_json_keys_recursive(c);
    }
    return item;
}

static void sha256_to_hex(const unsigned char *data, size_t len, char *hex_out)
{
    unsigned char hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), data, len, hash);
    bytes_to_hex(hash, 32, hex_out);
}

static void hash_cjson(const cJSON *item, char *hex_out)
{
    char *s = cJSON_PrintUnformatted(item);
    if (s) {
        sha256_to_hex((const unsigned char *)s, strlen(s), hex_out);
        cJSON_free(s);
    }
}

static bool is_set_address_io(cJSON *io)
{
    cJSON *n = cJSON_GetObjectItem(io, "IO_name");
    const char *ns = (n && n->valuestring) ? n->valuestring : io->string;
    return ns && strcmp(ns, "Set_Address") == 0;
}

// ---------------------------------------------------------------------------
// Core load: takes ownership of root and atomically swaps g_config
// ---------------------------------------------------------------------------

static esp_err_t load_from_cjson(cJSON *root)
{
    if (!root) return ESP_ERR_INVALID_ARG;

    sort_json_keys_recursive(root);

    cJSON *units_json = cJSON_GetObjectItem(root, "units");
    cJSON *cu_json    = cJSON_GetObjectItem(root, "configure_units");

    // Count first so the config takes exactly 3 allocations
    int total_ios  = 0;
    int unit_count = 0;
    if (cJSON_IsObject(units_json)) {
        cJSON *uj;
        cJSON_ArrayForEach(uj, units_json) {
            if (unit_count >= MAX_UNITS) break;
            cJSON *io_cfg = cJSON_GetObjectItem(uj, "IO_config");
            if (cJSON_IsObject(io_cfg)) {
                cJSON *io;
                cJSON_ArrayForEach(io, io_cfg) {
                    if (!is_set_address_io(io)) total_ios++;
                }
            }
            unit_count++;
        }
    }
    int cu_count = cJSON_IsObject(cu_json) ? cJSON_GetArraySize(cu_json) : 0;
    if (cu_count > MAX_CONFIGURE_UNITS) cu_count = MAX_CONFIGURE_UNITS;

    system_config_t *new_cfg = cm_calloc(1, sizeof(system_config_t));
    if (!new_cfg) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }

    if (total_ios > 0) {
        new_cfg->io_pool = cm_calloc(total_ios, sizeof(io_config_t));
        if (!new_cfg->io_pool) {
            free_config_struct(new_cfg); cJSON_Delete(root); return ESP_ERR_NO_MEM;
        }
    }
    if (cu_count > 0) {
        new_cfg->configure_units = cm_calloc(cu_count, sizeof(configure_unit_t));
        if (!new_cfg->configure_units) {
            free_config_struct(new_cfg); cJSON_Delete(root); return ESP_ERR_NO_MEM;
        }
    }

    cJSON *tmp;
    new_cfg->version = (tmp = cJSON_GetObjectItem(root, "config_version")) ? tmp->valueint : 0;
    if ((tmp = cJSON_GetObjectItem(root, "name")) && tmp->valuestring)
        strncpy(new_cfg->name, tmp->valuestring, MAX_NAME_LEN - 1);
    if ((tmp = cJSON_GetObjectItem(root, "device_ID")) && tmp->valuestring)
        strncpy(new_cfg->device_id, tmp->valuestring, MAX_NAME_LEN - 1);

    int io_pool_idx = 0;
    if (cJSON_IsObject(units_json)) {
        cJSON *uj;
        cJSON_ArrayForEach(uj, units_json) {
            if (new_cfg->unit_count >= MAX_UNITS) break;

            unit_config_t *u = &new_cfg->units[new_cfg->unit_count];
            strncpy(u->name, uj->string ? uj->string : "unknown", MAX_NAME_LEN - 1);
            u->address          = (tmp = cJSON_GetObjectItem(uj, "unit_address"))         ? (uint8_t)tmp->valueint  : 0;
            u->wifi_interval_s  = (tmp = cJSON_GetObjectItem(uj, "unit_wifi_interval"))   ? (uint16_t)tmp->valueint : 60;
            u->nbiot_interval_s = (tmp = cJSON_GetObjectItem(uj, "unit_NB-IOT_interval")) ? (uint16_t)tmp->valueint : 300;
            u->io_pool_offset   = io_pool_idx;
            u->io_count         = 0;

            cJSON *io_cfg = cJSON_GetObjectItem(uj, "IO_config");
            if (cJSON_IsObject(io_cfg)) {
                cJSON *io;
                cJSON_ArrayForEach(io, io_cfg) {
                    if (is_set_address_io(io)) continue;
                    parse_io(io, &new_cfg->io_pool[io_pool_idx++]);
                    u->io_count++;
                }
            }

            hash_cjson(uj, u->hash);
            new_cfg->unit_count++;
        }
    }
    new_cfg->io_pool_count = io_pool_idx;

    if (cJSON_IsObject(cu_json)) {
        cJSON *cui;
        cJSON_ArrayForEach(cui, cu_json) {
            if (new_cfg->configure_unit_count >= MAX_CONFIGURE_UNITS) break;
            configure_unit_t *cu = &new_cfg->configure_units[new_cfg->configure_unit_count++];
            strncpy(cu->sensor_type, cui->string ? cui->string : "unknown", MAX_NAME_LEN - 1);
            cu->address_register = (uint16_t)cui->valueint;
        }
    }

    hash_cjson(root, new_cfg->system_hash);
    if (cu_json) hash_cjson(cu_json, new_cfg->configure_units_hash);

    cJSON_Delete(root);

    if (xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free_config_struct(new_cfg);
        return ESP_ERR_TIMEOUT;
    }
    free_config_struct(g_config);
    g_config = new_cfg;
    xSemaphoreGiveRecursive(s_config_mux);

    ESP_LOGI(TAG, "Config loaded: v%d, %d units, %d IOs. Free heap: %zu",
             new_cfg->version, new_cfg->unit_count, new_cfg->io_pool_count, free_heap());
    return ESP_OK;
}

static esp_err_t load_from_string(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse config JSON");
        return ESP_FAIL;
    }
    return load_from_cjson(root);
}

static void drop_active_config(void)
{
    config_manager_lock();
    free_config_struct(g_config);
    g_config = NULL;
    config_manager_unlock();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

esp_err_t config_manager_init(void)
{
    if (!s_file_mux)   s_file_mux   = xSemaphoreCreateRecursiveMutex();
    if (!s_config_mux) s_config_mux = xSemaphoreCreateRecursiveMutex();
    if (!s_file_mux || !s_config_mux) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Config manager initialized (schema v%d, file: %s)", CONFIG_MANAGER_SCHEMA_VERSION, CONFIG_MANAGER_FILE_PATH);
    return ESP_OK;
}

esp_err_t config_manager_deinit(void)
{
    if (!s_config_mux) return ESP_OK;
    drop_active_config();
    vSemaphoreDelete(s_config_mux);
    vSemaphoreDelete(s_file_mux);
    s_config_mux = NULL;
    s_file_mux = NULL;
    return ESP_OK;
}

esp_err_t config_manager_load(void)
{
    if (!s_file_mux) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(s_file_mux, pdMS_TO_TICKS(5000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    char *json_str = read_file(CONFIG_MANAGER_FILE_PATH);
    xSemaphoreGiveRecursive(s_file_mux);

    if (!json_str) {
        ESP_LOGW(TAG, "No config at %s, writing factory defaults", CONFIG_MANAGER_FILE_PATH);
        return config_manager_reset_to_default(NULL);
    }
    esp_err_t ret = load_from_string(json_str);
    cm_free(json_str);
    return ret;
}

esp_err_t config_manager_save(const char *json_data)
{
    if (!json_data) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        ESP_LOGE(TAG, "Rejected config: invalid JSON, file not changed");
        return ESP_FAIL;
    }
    if (!write_config_file(json_data)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    return load_from_cjson(root);
}

// ---------------------------------------------------------------------------
// Locked access
// ---------------------------------------------------------------------------

const system_config_t *config_manager_get_ptr(void)
{
    if (!s_config_mux || xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(1000)) != pdTRUE)
        return NULL;
    if (!g_config) {
        // The caller must not call config_manager_release() for NULL
        xSemaphoreGiveRecursive(s_config_mux);
        return NULL;
    }
    return g_config;
}

void config_manager_release(void)
{
    xSemaphoreGiveRecursive(s_config_mux);
}

void config_manager_unload(void)
{
    if (xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to lock config for unloading");
        return;
    }
    if (g_config) {
        free_config_struct(g_config);
        g_config = NULL;
        ESP_LOGI(TAG, "Configuration unloaded from RAM");
    }
    xSemaphoreGiveRecursive(s_config_mux);
}

void config_manager_lock(void)
{
    xSemaphoreTakeRecursive(s_config_mux, portMAX_DELAY);
}

void config_manager_unlock(void)
{
    xSemaphoreGiveRecursive(s_config_mux);
}

// Taking and releasing the lock from a task that does not hold it waits for any holder (e.g. a Modbus scan).
esp_err_t config_manager_await_idle(uint32_t timeout_ms)
{
    if (xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    xSemaphoreGiveRecursive(s_config_mux);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Read helpers
// ---------------------------------------------------------------------------

int config_manager_get_version(void)
{
    if (!s_config_mux || xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(100)) != pdTRUE)
        return 0;
    int ver = g_config ? g_config->version : 0;
    xSemaphoreGiveRecursive(s_config_mux);
    return ver;
}

// The pointer stays valid only until the next load/update; hold config_manager_lock() if that matters.
const char *config_manager_get_string(const char *key)
{
    if (!key || !g_config) return NULL;
    if (strcmp(key, "name")      == 0) return g_config->name;
    if (strcmp(key, "device_ID") == 0) return g_config->device_id;
    return NULL;
}

cJSON *config_manager_get_all_unit_hashes(void)
{
    const system_config_t *cfg = config_manager_get_ptr();
    if (!cfg) return NULL;
    cJSON *hashes = cJSON_CreateObject();
    if (hashes) {
        // Keyed by the unit's JSON key (e.g. "unit_2"), which is what handle_sync compares against
        for (int i = 0; i < cfg->unit_count; i++) {
            cJSON_AddStringToObject(hashes, cfg->units[i].name, cfg->units[i].hash);
        }
    }
    config_manager_release();
    return hashes;
}

esp_err_t config_manager_compute_configure_units_hash(char *hash_out)
{
    if (!hash_out) return ESP_ERR_INVALID_ARG;
    const system_config_t *cfg = config_manager_get_ptr();
    if (!cfg) return ESP_FAIL;
    strncpy(hash_out, cfg->configure_units_hash, MAX_HASH_LEN);
    config_manager_release();
    return ESP_OK;
}

cJSON *config_manager_get_config_json(void)
{
    return load_config_file(NULL);
}

cJSON *config_manager_get_unit_json(const char *unit_key)
{
    cJSON *root = config_manager_get_config_json();
    if (!root) return NULL;
    cJSON *unit = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "units"), unit_key);
    cJSON *copy = unit ? cJSON_Duplicate(unit, 1) : NULL;
    cJSON_Delete(root);
    return copy;
}

// One file read for all requested units: {"units":["unit_1",...], "configure_units":true}
char *config_manager_get_units_payload(const char *request_json)
{
    if (!request_json) return NULL;
    cJSON *request = cJSON_Parse(request_json);
    if (!request) return NULL;

    cJSON *units_req = cJSON_GetObjectItem(request, "units");
    if (!cJSON_IsArray(units_req)) {
        cJSON_Delete(request);
        return NULL;
    }

    cJSON *response = cJSON_CreateObject();
    if (!response) { cJSON_Delete(request); return NULL; }

    cJSON_AddNumberToObject(response, "config_version", config_manager_get_version());
    cJSON *units_resp = cJSON_CreateObject();
    cJSON_AddItemToObject(response, "units", units_resp);

    cJSON *full_cfg  = config_manager_get_config_json();
    cJSON *all_units = full_cfg ? cJSON_GetObjectItem(full_cfg, "units") : NULL;

    cJSON *unit_key;
    cJSON_ArrayForEach(unit_key, units_req) {
        if (!cJSON_IsString(unit_key)) continue;
        cJSON *unit_data = all_units ? cJSON_GetObjectItem(all_units, unit_key->valuestring) : NULL;
        if (unit_data) {
            cJSON_AddItemToObject(units_resp, unit_key->valuestring, cJSON_Duplicate(unit_data, 1));
        } else {
            ESP_LOGW(TAG, "Requested unit '%s' not found in config", unit_key->valuestring);
        }
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(request, "configure_units")) && full_cfg) {
        cJSON *cu = cJSON_GetObjectItem(full_cfg, "configure_units");
        if (cu) cJSON_AddItemToObject(response, "configure_units", cJSON_Duplicate(cu, 1));
    }

    cJSON_Delete(full_cfg);
    char *out_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(request);
    cJSON_Delete(response);
    return out_str;
}

// ---------------------------------------------------------------------------
// Update handlers
// ---------------------------------------------------------------------------

static bool write_config_file(const char *json_str)
{
    if (!s_file_mux || xSemaphoreTakeRecursive(s_file_mux, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
    FILE *f = fopen(CONFIG_MANAGER_FILE_PATH, "w");
    bool ok = false;
    size_t len = strlen(json_str);
    if (f) {
        ok = (fwrite(json_str, 1, len, f) == len);
        fclose(f);
    }
    xSemaphoreGiveRecursive(s_file_mux);
    if (ok) {
        ESP_LOGD(TAG, "FILE_WRITE: %zu bytes written to %s", len, CONFIG_MANAGER_FILE_PATH);
        ESP_LOGV(TAG, "FILE_WRITE raw: %s", json_str);
    } else {
        ESP_LOGE(TAG, "FILE_WRITE: failed to write %s", CONFIG_MANAGER_FILE_PATH);
    }
    return ok;
}

bool config_manager_handle_partial_update(const char *json_data)
{
    if (!json_data) return false;

    cJSON *incoming = cJSON_Parse(json_data);
    if (!incoming) {
        ESP_LOGE(TAG, "PARTIAL_UPDATE: Failed to parse incoming JSON");
        return false;
    }
    ESP_LOGV(TAG, "Config received: %s", json_data);

    // Reject stale updates before touching anything
    int current_ver = config_manager_get_version();
    cJSON *in_ver_item = cJSON_GetObjectItem(incoming, "config_version");
    int in_ver = cJSON_IsNumber(in_ver_item) ? in_ver_item->valueint : 0;
    if (in_ver <= current_ver) {
        ESP_LOGI(TAG, "PARTIAL_UPDATE: Skipped (incoming=%d <= current=%d)", in_ver, current_ver);
        cJSON_Delete(incoming);
        return false;
    }

    // Free the active config first to maximise heap for the merge; readers handle g_config == NULL
    drop_active_config();
    ESP_LOGD(TAG, "PARTIAL_UPDATE: Freed active config. Free heap: %zu", free_heap());

    long file_json_size = 0;
    cJSON *root = load_config_file(&file_json_size);
    if (!root) {
        ESP_LOGW(TAG, "PARTIAL_UPDATE: No config file, writing incoming as-is");
        bool ok = write_config_file(json_data) && (load_from_string(json_data) == ESP_OK);
        cJSON_Delete(incoming);
        if (!ok) config_manager_load();
        return ok;
    }

    int added = 0, updated = 0, deleted = 0;

    cJSON_ReplaceItemInObject(root, "config_version", cJSON_CreateNumber(in_ver));

    // Root scalars the server may send; detach from incoming (move, no copy)
    static const char *const scalar_keys[] = {"name", "gps_enabled", "device_ID", NULL};
    for (int k = 0; scalar_keys[k]; k++) {
        cJSON *field = cJSON_DetachItemFromObject(incoming, scalar_keys[k]);
        if (field) {
            cJSON_DeleteItemFromObject(root, scalar_keys[k]);
            cJSON_AddItemToObject(root, scalar_keys[k], field);
        }
    }

    cJSON *current_units = cJSON_GetObjectItem(root, "units");
    if (!current_units) {
        current_units = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "units", current_units);
    }

    cJSON *del_arr = cJSON_GetObjectItem(incoming, "delete_units");
    if (cJSON_IsArray(del_arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, del_arr) {
            if (cJSON_IsString(item) && cJSON_HasObjectItem(current_units, item->valuestring)) {
                cJSON_DeleteItemFromObject(current_units, item->valuestring);
                deleted++;
            }
        }
    }

    cJSON *in_units = cJSON_DetachItemFromObject(incoming, "units");
    if (cJSON_IsObject(in_units)) {
        cJSON *unit;
        while ((unit = cJSON_DetachItemFromArray(in_units, 0)) != NULL) {
            if (cJSON_HasObjectItem(current_units, unit->string)) {
                cJSON_DeleteItemFromObject(current_units, unit->string);
                updated++;
            } else {
                added++;
            }
            cJSON_AddItemToObject(current_units, unit->string, unit);
        }
    }
    cJSON_Delete(in_units);

    cJSON *in_cu = cJSON_DetachItemFromObject(incoming, "configure_units");
    if (in_cu) {
        cJSON_DeleteItemFromObject(root, "configure_units");
        cJSON_AddItemToObject(root, "configure_units", in_cu);
    }

    cJSON_Delete(incoming);

    long estimated_size = file_json_size + (long)strlen(json_data);
    char *out_str = serialize_cjson(root, estimated_size);
    if (!out_str) {
        ESP_LOGE(TAG, "PARTIAL_UPDATE: serialisation OOM (heap %zu)", free_heap());
        cJSON_Delete(root);
        config_manager_load();
        return false;
    }

    bool file_ok = write_config_file(out_str);
    cm_free(out_str);
    if (!file_ok)
        ESP_LOGE(TAG, "PARTIAL_UPDATE: file write failed, config active in RAM only");

    // Build from the merged tree directly; avoids parsing the string a second time
    bool success = (load_from_cjson(root) == ESP_OK);

    ESP_LOGI(TAG, "PARTIAL_UPDATE: v%d->%d | +%d ~%d -%d | success=%d",
             current_ver, in_ver, added, updated, deleted, success);
    return success;
}

bool config_manager_delete_unit(const char *unit_key)
{
    if (!unit_key) return false;

    drop_active_config();

    long file_size = 0;
    cJSON *root = load_config_file(&file_size);
    if (!root) { ESP_LOGE(TAG, "DELETE_UNIT: Failed to load config"); return false; }

    cJSON *units = cJSON_GetObjectItem(root, "units");
    if (!cJSON_IsObject(units) || !cJSON_HasObjectItem(units, unit_key)) {
        ESP_LOGW(TAG, "DELETE_UNIT: Unit '%s' not found", unit_key);
        cJSON_Delete(root);
        config_manager_load();
        return false;
    }

    cJSON_DeleteItemFromObject(units, unit_key);

    char *out_str = serialize_cjson(root, file_size);
    cJSON_Delete(root);
    if (!out_str) {
        ESP_LOGE(TAG, "DELETE_UNIT: serialisation OOM");
        config_manager_load();
        return false;
    }

    esp_err_t err = config_manager_save(out_str);
    cm_free(out_str);
    if (err != ESP_OK) config_manager_load();
    return err == ESP_OK;
}

bool config_manager_handle_sync(const char *json_data)
{
    if (!json_data) return false;

    cJSON *sync_msg = cJSON_Parse(json_data);
    if (!sync_msg) { ESP_LOGE(TAG, "CFG_SYNC: Failed to parse sync JSON"); return false; }

    const system_config_t *cfg = config_manager_get_ptr();
    if (!cfg) {
        ESP_LOGE(TAG, "CFG_SYNC: Config not loaded");
        cJSON_Delete(sync_msg);
        return false;
    }
    ESP_LOGI(TAG, "CFG_SYNC: Starting sync check...");

    bool needs_hello = false;

    cJSON *ver_item = cJSON_GetObjectItem(sync_msg, "config_version");
    if (cJSON_IsNumber(ver_item) && cfg->version != ver_item->valueint) {
        ESP_LOGI(TAG, "CFG_SYNC: Version mismatch (local=%d, server=%d)", cfg->version, ver_item->valueint);
        needs_hello = true;
    }

    cJSON *server_hashes = cJSON_GetObjectItem(sync_msg, "units_hash");
    if (cJSON_IsObject(server_hashes)) {
        cJSON *sh;
        cJSON_ArrayForEach(sh, server_hashes) {
            bool found = false;
            for (int i = 0; i < cfg->unit_count; i++) {
                if (strcmp(cfg->units[i].name, sh->string) == 0) {
                    found = true;
                    if (!cJSON_IsString(sh) || strcmp(cfg->units[i].hash, sh->valuestring) != 0) {
                        ESP_LOGI(TAG, "CFG_SYNC: Unit '%s' hash mismatch", sh->string);
                        needs_hello = true;
                    }
                    break;
                }
            }
            if (!found) {
                ESP_LOGI(TAG, "CFG_SYNC: Server unit '%s' missing on device", sh->string);
                needs_hello = true;
            }
        }
        for (int i = 0; i < cfg->unit_count; i++) {
            if (!cJSON_HasObjectItem(server_hashes, cfg->units[i].name)) {
                ESP_LOGI(TAG, "CFG_SYNC: Device has extra unit '%s'", cfg->units[i].name);
                needs_hello = true;
            }
        }
    }

    cJSON *server_cu_hash = cJSON_GetObjectItem(sync_msg, "configure_units_hash");
    if (cJSON_IsString(server_cu_hash) && strcmp(cfg->configure_units_hash, server_cu_hash->valuestring) != 0) {
        ESP_LOGI(TAG, "CFG_SYNC: configure_units_hash mismatch");
        needs_hello = true;
    }

    config_manager_release();

    bool units_deleted = false;
    cJSON *delete_units = cJSON_GetObjectItem(sync_msg, "delete_units");
    if (cJSON_IsArray(delete_units)) {
        cJSON *k;
        cJSON_ArrayForEach(k, delete_units) {
            if (cJSON_IsString(k) && config_manager_delete_unit(k->valuestring)) {
                ESP_LOGI(TAG, "CFG_SYNC: Deleted unit '%s'", k->valuestring);
                units_deleted = true;
            }
        }
    }

    cJSON_Delete(sync_msg);

    bool out_of_sync = needs_hello || units_deleted;
    ESP_LOGI(TAG, "CFG_SYNC: %s", out_of_sync ? "Out of sync" : "In sync");
    return out_of_sync;
}

// ---------------------------------------------------------------------------
// Live values
// ---------------------------------------------------------------------------

static io_config_t *find_io_locked(const char *io_name, uint8_t unit_idx)
{
    if (!g_config || unit_idx >= (uint8_t)g_config->unit_count) return NULL;
    const unit_config_t *unit = &g_config->units[unit_idx];
    for (int i = 0; i < unit->io_count; i++) {
        io_config_t *io = &g_config->io_pool[unit->io_pool_offset + i];
        if (strcmp(io->name, io_name) == 0) return io;
    }
    return NULL;
}

esp_err_t config_update_live_value(const char *io_name, uint8_t unit_idx, float new_val)
{
    if (!io_name) return ESP_ERR_INVALID_ARG;
    if (!s_config_mux || xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (g_config && unit_idx < (uint8_t)g_config->unit_count) {
        io_config_t *io = find_io_locked(io_name, unit_idx);
        if (io) {
            io->live_value = new_val;
            ESP_LOGV(TAG, "Live: %s[%d] = %.3f", io_name, unit_idx, new_val);
            ret = ESP_OK;
        } else {
            ret = ESP_ERR_NOT_FOUND;
        }
    }
    xSemaphoreGiveRecursive(s_config_mux);
    return ret;
}

float config_get_live_value(const char *io_name, uint8_t unit_idx)
{
    if (!io_name) return NAN;
    if (!s_config_mux || xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(100)) != pdTRUE) return NAN;
    const io_config_t *io = find_io_locked(io_name, unit_idx);
    float v = io ? io->live_value : NAN;
    xSemaphoreGiveRecursive(s_config_mux);
    return v;
}

// ---------------------------------------------------------------------------
// Factory reset
// ---------------------------------------------------------------------------

esp_err_t config_manager_reset_to_default(const char *device_id)
{
    ESP_LOGI(TAG, "Resetting to factory defaults...");

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(root, "config_version", 1);
    cJSON_AddStringToObject(root, "device_ID", device_id ? device_id : "");
    cJSON_AddStringToObject(root, "name", "MIOT-Gateway");
    cJSON_AddBoolToObject(root, "gps_enabled", false);
    cJSON_AddItemToObject(root, "units", cJSON_CreateObject());

    cJSON *cu = cJSON_CreateObject();
    if (cu) {
        cJSON_AddNumberToObject(cu, "MIOT_Level_Sensor", 298);
        cJSON_AddNumberToObject(cu, "MIOT_Trigger", 299);
        cJSON_AddItemToObject(root, "configure_units", cu);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    // Drop the active config now so nothing stale lingers even if the write fails
    if (s_config_mux) drop_active_config();

    esp_err_t ret = config_manager_save(json_str);
    cJSON_free(json_str);

    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Factory reset complete, default config loaded (0 units)");
    else
        ESP_LOGE(TAG, "Factory reset: file write failed, config cleared; device should restart");
    return ret;
}
