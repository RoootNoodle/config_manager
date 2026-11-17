#include "config_manager.h"
#include "config_manager_priv.h"
#include "cm_alloc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <inttypes.h>
#include <errno.h>
#include <stdatomic.h>

static const char *TAG = "CONFIG_MGR";

#define MAX_IOS         CONFIG_CONFIG_MANAGER_MAX_IOS
#define MAX_FILE_SIZE   CONFIG_CONFIG_MANAGER_MAX_FILE_SIZE
#define IO_NAME_MAX     31   // plan section 2: IO name at most 31 characters
#define GW_NAME_MAX     39   // plan section 2: gateway name at most 39 characters
#define LOCK_TIMEOUT_MS 5000
#define DEFAULT_NAME    "MIOT-Gateway"
#define DEFAULT_WIFI_S  60
#define DEFAULT_NBIOT_S 300

// Protects the config file
static SemaphoreHandle_t s_file_mux   = NULL;
// Protects g_config, io_pool contents and s_patch_result (recursive: Modbus re-enters it for live writes)
static SemaphoreHandle_t s_config_mux = NULL;

static system_config_t *g_config = NULL;

// REVIEW: build_state(false) has no other way to learn what the last apply_patch did, so it is kept here until reported once.
static struct {
    bool        pending;
    const char *err;            // NULL when the patch was applied
    uint8_t     changed[32];    // bitmap by unit key
    uint8_t     deleted[32];
} s_patch_result;

static const char *const ROOT_KEYS[] = {"s", "v", "id", "n", "gps", "cu", "u", NULL};
static const char *const UNIT_KEYS[] = {"n", "ty", "a", "w", "c", "io", NULL};
static const char *const IO_KEYS[]   = {"r", "t", "f", "b", "l", "e", NULL};
static const char *const VTYPE_NAMES[] = {
    [IO_VTYPE_UINT16] = "u16", [IO_VTYPE_INT16] = "i16", [IO_VTYPE_UINT32] = "u32",
    [IO_VTYPE_INT32]  = "i32", [IO_VTYPE_FP32]  = "f32", [IO_VTYPE_BOOL]   = "bool",
};

static esp_err_t load_locked(const char *known_id);

// REVIEW: set while the RAM config is the default standing in for an unreadable file; apply_patch then merges onto that default.
static bool s_file_bad = false;
// REVIEW: set once the application asked for a config (load/save/reset); while set, a NULL g_config is retried, never permanent.
// REVIEW: atomic because config_manager_load() sets it before it has the lock (a lock timeout must still lead to a retry).
static atomic_bool s_config_wanted = false;
// Device ID of the last config installed, for the fallback and the retry after g_config was lost
static char s_device_id[MAX_NAME_LEN] = "";

static size_t free_heap(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}

static bool take_config(uint32_t timeout_ms)
{
    return s_config_mux && xSemaphoreTakeRecursive(s_config_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void give_config(void)
{
    xSemaphoreGiveRecursive(s_config_mux);
}

static void free_config_struct(system_config_t *cfg)
{
    if (!cfg) return;
    cm_free(cfg->io_pool);
    cm_free(cfg->configure_units);
    cm_free(cfg);
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

// REVIEW: status tells "missing" (ESP_ERR_NOT_FOUND) apart from empty/unreadable and out of memory, so OOM never looks like a missing file.
static char *read_file(const char *path, size_t *len_out, esp_err_t *status)
{
    esp_err_t st = ESP_OK;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (status) *status = (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        if (status) *status = ESP_ERR_INVALID_SIZE;
        return NULL;
    }
    // REVIEW: checked before allocating, so an oversized file is "unusable" (fallback) instead of an endless out-of-memory.
    if (size > MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "%s is %ld B, over CONFIG_MANAGER_MAX_FILE_SIZE (%d)", path, size, MAX_FILE_SIZE);
        fclose(f);
        if (status) *status = ESP_ERR_INVALID_SIZE;
        return NULL;
    }

    char *buf = cm_malloc(size + 1);
    if (buf) {
        size_t n = fread(buf, 1, size, f);
        buf[n] = '\0';
        if (len_out) *len_out = n;
    } else {
        ESP_LOGE(TAG, "No memory for %ld B file (heap %zu)", size, free_heap());
        st = ESP_ERR_NO_MEM;
    }
    fclose(f);
    if (status) *status = st;
    return buf;
}

static char *read_config_text(size_t *len_out, esp_err_t *status)
{
    if (!s_file_mux || xSemaphoreTakeRecursive(s_file_mux, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) != pdTRUE) {
        if (status) *status = ESP_ERR_TIMEOUT;
        return NULL;
    }
    char *text = read_file(CONFIG_MANAGER_FILE_PATH, len_out, status);
    xSemaphoreGiveRecursive(s_file_mux);
    return text;
}

// REVIEW: the file text is freed right after parsing so text and tree do not stay allocated together.
static cJSON *load_config_file(void)
{
    char *text = read_config_text(NULL, NULL);
    if (!text) return NULL;
    cJSON *root = cJSON_Parse(text);
    cm_free(text);
    return root;
}

static esp_err_t write_config_file(const char *json_str)
{
    if (!s_file_mux || xSemaphoreTakeRecursive(s_file_mux, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    FILE *f = fopen(CONFIG_MANAGER_FILE_PATH, "w");
    bool ok = false;
    size_t len = strlen(json_str);
    if (f) {
        ok = (fwrite(json_str, 1, len, f) == len);
        ok = (fclose(f) == 0) && ok;
    }
    xSemaphoreGiveRecursive(s_file_mux);
    if (!ok) {
        ESP_LOGE(TAG, "FILE_WRITE: failed to write %s", CONFIG_MANAGER_FILE_PATH);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "FILE_WRITE: %zu bytes written to %s", len, CONFIG_MANAGER_FILE_PATH);
    return ESP_OK;
}

// REVIEW: plan section 6 step 4; called only after a v2 config from save/patch was written and activated.
static void remove_v1_file(void)
{
    static const char v1_path[] = CONFIG_MANAGER_V1_FILE_PATH;
    if (v1_path[0] == '\0' || strcmp(v1_path, CONFIG_MANAGER_FILE_PATH) == 0) return;
    if (!s_file_mux || xSemaphoreTakeRecursive(s_file_mux, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) != pdTRUE) return;
    if (remove(v1_path) == 0) ESP_LOGI(TAG, "Deleted v1 config %s", v1_path);
    xSemaphoreGiveRecursive(s_file_mux);
}

// REVIEW: a fixed buffer of MAX_FILE_SIZE both enforces the size quota and avoids cJSON's realloc doubling.
static esp_err_t serialize_bounded(const cJSON *tree, char **out)
{
    // cJSON_PrintPreallocated needs a few spare bytes beyond the output
    const int buf_size = MAX_FILE_SIZE + 6;
    char *buf = cm_malloc(buf_size);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for %d B output buffer (heap %zu)", buf_size, free_heap());
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_PrintPreallocated((cJSON *)tree, buf, buf_size, 0) || strlen(buf) > MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "Rejected config: larger than %d B (quota)", MAX_FILE_SIZE);
        cm_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    *out = buf;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Validation and parsing (schema v2)
// ---------------------------------------------------------------------------

static bool get_int(const cJSON *item, long min, long max, long *out)
{
    if (!cJSON_IsNumber(item)) return false;
    double d = item->valuedouble;
    if (!(d >= (double)min && d <= (double)max)) return false;
    long l = (long)d;
    if ((double)l != d) return false;   // whole numbers only (plan section 5)
    *out = l;
    return true;
}

static bool keys_allowed(const cJSON *obj, const char *const *allowed, const char *where)
{
    for (const cJSON *c = obj->child; c; c = c->next) {
        bool ok = false;
        for (int i = 0; allowed[i]; i++) {
            if (strcmp(c->string, allowed[i]) == 0) { ok = true; break; }
        }
        if (!ok) {
            ESP_LOGW(TAG, "Rejected: unknown key '%s' in %s", c->string, where);
            return false;
        }
    }
    return true;
}

// REVIEW: cJSON keeps duplicate keys; reject them so the hash and the parser cannot see different values.
static bool has_duplicate_keys(const cJSON *obj, const char *where)
{
    for (const cJSON *a = obj->child; a; a = a->next) {
        for (const cJSON *b = a->next; b; b = b->next) {
            if (strcmp(a->string, b->string) == 0) {
                ESP_LOGW(TAG, "Rejected: duplicate key '%s' in %s", a->string, where);
                return true;
            }
        }
    }
    return false;
}

static bool object_ok(const cJSON *obj, const char *const *allowed, const char *where)
{
    if (!cJSON_IsObject(obj)) {
        ESP_LOGW(TAG, "Rejected: %s is not an object", where);
        return false;
    }
    if (has_duplicate_keys(obj, where)) return false;
    return !allowed || keys_allowed(obj, allowed, where);
}

// REVIEW: only canonical "1"-"255" (no leading zero), so a key string maps to exactly one number.
static bool parse_unit_key(const char *s, uint8_t *out)
{
    if (!s || s[0] < '1' || s[0] > '9') return false;
    int v = 0;
    for (int i = 0; s[i]; i++) {
        if (i >= 3 || s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    if (v > 255) return false;
    *out = (uint8_t)v;
    return true;
}

static bool parse_vtype(const char *s, io_value_type_t *out)
{
    if (!s) return false;
    for (int i = 0; i < (int)(sizeof(VTYPE_NAMES) / sizeof(VTYPE_NAMES[0])); i++) {
        if (strcmp(s, VTYPE_NAMES[i]) == 0) { *out = (io_value_type_t)i; return true; }
    }
    return false;
}

// REVIEW: IO names and the device ID end up in MQTT topics (sense/control, devices/{id}), so topic separators,
// wildcards and control characters are not allowed in them.
static bool topic_safe(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '/' || c == '+' || c == '#' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static bool is_write_fc(long fc) { return fc == 5 || fc == 6 || fc == 15 || fc == 16; }

static esp_err_t parse_io(const cJSON *io_json, io_config_t *io)
{
    const char *name = io_json->string;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > IO_NAME_MAX) {
        ESP_LOGW(TAG, "Rejected: IO name '%s' must be 1..%d bytes", name, IO_NAME_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (!topic_safe(name)) {
        ESP_LOGW(TAG, "Rejected: IO name '%s' contains '/', '+', '#' or a control character", name);
        return ESP_ERR_INVALID_ARG;
    }
    if (!object_ok(io_json, IO_KEYS, name)) return ESP_ERR_INVALID_ARG;

    memset(io, 0, sizeof(*io));
    memcpy(io->name, name, name_len + 1);
    io->live_value = NAN;

    long v;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(io_json, "r"), 0, 65535, &v)) {
        ESP_LOGW(TAG, "Rejected: IO '%s': r missing or not 0..65535", name);
        return ESP_ERR_INVALID_ARG;
    }
    io->read_addr = (uint16_t)v;

    if (!parse_vtype(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(io_json, "t")), &io->value_type)) {
        ESP_LOGW(TAG, "Rejected: IO '%s': t missing or unknown", name);
        return ESP_ERR_INVALID_ARG;
    }

    // REVIEW: f is required; at most one read code and one write code (plan section 2).
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(io_json, "f");
    int n_codes = cJSON_IsArray(f) ? cJSON_GetArraySize(f) : 0;
    if (n_codes < 1 || n_codes > 2) {
        ESP_LOGW(TAG, "Rejected: IO '%s': f must be an array of 1 or 2 codes", name);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *c;
    cJSON_ArrayForEach(c, f) {
        if (get_int(c, 1, 4, &v) && !io->read_fc)            io->read_fc  = (uint8_t)v;
        else if (get_int(c, 5, 16, &v) && is_write_fc(v) && !io->write_fc) io->write_fc = (uint8_t)v;
        else {
            ESP_LOGW(TAG, "Rejected: IO '%s': bad or repeated function code in f", name);
            return ESP_ERR_INVALID_ARG;
        }
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(io_json, "b");
    if (item && !get_int(item, 0, 255, &v)) {
        ESP_LOGW(TAG, "Rejected: IO '%s': b not 0..255", name);
        return ESP_ERR_INVALID_ARG;
    }
    io->start_byte = item ? (uint8_t)v : 0;

    item = cJSON_GetObjectItemCaseSensitive(io_json, "l");
    if (item && !get_int(item, 1, 8, &v)) {
        ESP_LOGW(TAG, "Rejected: IO '%s': l not 1..8", name);
        return ESP_ERR_INVALID_ARG;
    }
    bool wide = io->value_type == IO_VTYPE_UINT32 || io->value_type == IO_VTYPE_INT32 || io->value_type == IO_VTYPE_FP32;
    io->length = item ? (uint8_t)v : (wide ? 4 : 2);

    // REVIEW: "b" is accepted as well as "l" because the hash writes the default out as "b".
    item = cJSON_GetObjectItemCaseSensitive(io_json, "e");
    const char *e = cJSON_GetStringValue(item);
    if (item && (!e || (strcmp(e, "l") != 0 && strcmp(e, "b") != 0))) {
        ESP_LOGW(TAG, "Rejected: IO '%s': e must be \"l\" or \"b\"", name);
        return ESP_ERR_INVALID_ARG;
    }
    io->big_endian = !(e && strcmp(e, "l") == 0);
    return ESP_OK;
}

// Child with the smallest key after prev (byte order); objects are small, so no sorted copy is made
static const cJSON *next_sorted(const cJSON *obj, const cJSON *prev)
{
    const cJSON *best = NULL;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (prev && strcmp(c->string, prev->string) <= 0) continue;
        if (!best || strcmp(c->string, best->string) < 0) best = c;
    }
    return best;
}

// u and io_pool may be NULL to validate only; IOs are written in name byte order from io_pool[0].
static esp_err_t parse_unit(const cJSON *uj, unit_config_t *u, io_config_t *io_pool, int *io_count)
{
    char where[16];
    snprintf(where, sizeof(where), "unit %s", uj->string);
    if (!object_ok(uj, UNIT_KEYS, where)) return ESP_ERR_INVALID_ARG;

    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(uj, "n"))) {
        ESP_LOGW(TAG, "Rejected: %s: n missing or not a string", where);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *ty = cJSON_GetObjectItemCaseSensitive(uj, "ty");
    if (ty && !cJSON_IsString(ty)) {
        ESP_LOGW(TAG, "Rejected: %s: ty not a string", where);
        return ESP_ERR_INVALID_ARG;
    }

    long a, w = DEFAULT_WIFI_S, c = DEFAULT_NBIOT_S;
    const cJSON *item;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(uj, "a"), 2, 247, &a)) {
        ESP_LOGW(TAG, "Rejected: %s: a missing or not 2..247", where);
        return ESP_ERR_INVALID_ARG;
    }
    // REVIEW: interval range 1..65535 (uint16_t field, v1 schema minimum 1); the plan gives no range.
    if ((item = cJSON_GetObjectItemCaseSensitive(uj, "w")) && !get_int(item, 1, 65535, &w)) {
        ESP_LOGW(TAG, "Rejected: %s: w not 1..65535", where);
        return ESP_ERR_INVALID_ARG;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(uj, "c")) && !get_int(item, 1, 65535, &c)) {
        ESP_LOGW(TAG, "Rejected: %s: c not 1..65535", where);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *ios = cJSON_GetObjectItemCaseSensitive(uj, "io");
    if (!ios) {
        ESP_LOGW(TAG, "Rejected: %s: io missing", where);
        return ESP_ERR_INVALID_ARG;
    }
    if (!object_ok(ios, NULL, where)) return ESP_ERR_INVALID_ARG;

    if (u) {
        u->address          = (uint8_t)a;
        u->wifi_interval_s  = (uint16_t)w;
        u->nbiot_interval_s = (uint16_t)c;
    }

    int n = 0;
    io_config_t scratch;
    for (const cJSON *io = next_sorted(ios, NULL); io; io = next_sorted(ios, io)) {
        esp_err_t ret = parse_io(io, io_pool ? &io_pool[n] : &scratch);
        if (ret != ESP_OK) return ret;
        n++;
    }
    *io_count = n;
    return ESP_OK;
}

// REVIEW: one function checks the whole v2 file; counts come back so build_config allocates exactly once.
static esp_err_t validate_config(const cJSON *root, int *unit_count, int *io_count, int *cu_count)
{
    if (!object_ok(root, ROOT_KEYS, "root")) return ESP_ERR_INVALID_ARG;

    long v;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "s"), 2, 2, &v)) {
        ESP_LOGW(TAG, "Rejected: s is not 2 (schema v%d file expected)", CONFIG_MANAGER_SCHEMA_VERSION);
        return ESP_ERR_INVALID_ARG;
    }
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "v"), 0, INT_MAX, &v)) {
        ESP_LOGW(TAG, "Rejected: v missing or not a whole number >= 0");
        return ESP_ERR_INVALID_ARG;
    }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id"));
    if (!id || !topic_safe(id)) {
        ESP_LOGW(TAG, "Rejected: id missing, not a string, or contains '/', '+', '#' or a control character");
        return ESP_ERR_INVALID_ARG;
    }
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "n"));
    // REVIEW: the 39 limit is counted in bytes, which is what name[40] can hold.
    if (!n || strlen(n) > GW_NAME_MAX) {
        ESP_LOGW(TAG, "Rejected: n missing, not a string or over %d bytes", GW_NAME_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "gps");
    if (item && !get_int(item, 0, 1, &v)) {
        ESP_LOGW(TAG, "Rejected: gps must be 0 or 1");
        return ESP_ERR_INVALID_ARG;
    }

    int cus = 0;
    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(root, "cu");
    if (cu) {
        if (!object_ok(cu, NULL, "cu")) return ESP_ERR_INVALID_ARG;
        const cJSON *e;
        cJSON_ArrayForEach(e, cu) {
            if (!get_int(e, 0, 65535, &v)) {
                ESP_LOGW(TAG, "Rejected: cu '%s' is not a register 0..65535", e->string);
                return ESP_ERR_INVALID_ARG;
            }
            cus++;
        }
    }

    const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "u");
    if (!units) {
        ESP_LOGW(TAG, "Rejected: u missing");
        return ESP_ERR_INVALID_ARG;
    }
    if (!object_ok(units, NULL, "u")) return ESP_ERR_INVALID_ARG;

    int n_units = 0, n_ios = 0;
    const cJSON *uj;
    cJSON_ArrayForEach(uj, units) {
        uint8_t key;
        if (!parse_unit_key(uj->string, &key)) {
            ESP_LOGW(TAG, "Rejected: unit key '%s' is not \"1\"..\"255\"", uj->string);
            return ESP_ERR_INVALID_ARG;
        }
        int ios = 0;
        esp_err_t ret = parse_unit(uj, NULL, NULL, &ios);
        if (ret != ESP_OK) return ret;
        n_ios += ios;
        n_units++;
    }

    // REVIEW: quota reported as ESP_ERR_INVALID_SIZE, which becomes "err":"q" (plan section 7).
    if (n_units > MAX_UNITS || n_ios > MAX_IOS || cus > MAX_CONFIGURE_UNITS) {
        ESP_LOGW(TAG, "Rejected: quota (units %d/%d, IOs %d/%d, cu %d/%d)",
                 n_units, MAX_UNITS, n_ios, MAX_IOS, cus, MAX_CONFIGURE_UNITS);
        return ESP_ERR_INVALID_SIZE;
    }
    *unit_count = n_units;
    *io_count   = n_ios;
    *cu_count   = cus;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Hashes (plan section 5): streamed into SHA-256, no canonical copy in RAM
// ---------------------------------------------------------------------------

typedef struct {
    mbedtls_md_context_t md;
    unsigned char        buf[64];
    size_t               len;
    int                  rc;
} hash_writer_t;

static void hw_flush(hash_writer_t *w)
{
    if (w->len && !w->rc) w->rc = mbedtls_md_update(&w->md, w->buf, w->len);
    w->len = 0;
}

static void hw_put(hash_writer_t *w, const char *s, size_t n)
{
    while (n--) {
        if (w->len == sizeof(w->buf)) hw_flush(w);
        w->buf[w->len++] = (unsigned char)*s++;
    }
}

static void hw_puts(hash_writer_t *w, const char *s) { hw_put(w, s, strlen(s)); }

static void hw_int(hash_writer_t *w, long v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%ld", v);
    hw_put(w, tmp, (size_t)n);
}

// REVIEW: same escaping as Python json.dumps(ensure_ascii=False): only ", \ and control characters.
static void hw_str(hash_writer_t *w, const char *s)
{
    hw_put(w, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        const char *esc = NULL;
        switch (ch) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        case '\b': esc = "\\b";  break;
        case '\f': esc = "\\f";  break;
        default: break;
        }
        if (esc) {
            hw_puts(w, esc);
        } else if (ch < 0x20) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
            hw_puts(w, tmp);
        } else {
            hw_put(w, s, 1);
        }
    }
    hw_put(w, "\"", 1);
}

static void hw_start(hash_writer_t *w)
{
    w->len = 0;
    if (!w->rc) w->rc = mbedtls_md_starts(&w->md);
}

static uint32_t hw_finish(hash_writer_t *w)
{
    unsigned char digest[32] = {0};
    hw_flush(w);
    if (!w->rc) w->rc = mbedtls_md_finish(&w->md, digest);
    return ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) | ((uint32_t)digest[2] << 8) | digest[3];
}

static void hash_to_hex(uint32_t hash, char out[9])
{
    snprintf(out, 9, "%08" PRIx32, hash);
}

// Keys in fixed byte order: unit a,c,io,n,ty,w; IO b,e,f,l,r,t. IOs are already sorted in io_pool.
static uint32_t hash_unit(hash_writer_t *w, const cJSON *uj, const unit_config_t *u, const io_config_t *ios)
{
    hw_start(w);
    hw_puts(w, "{\"a\":");    hw_int(w, u->address);
    hw_puts(w, ",\"c\":");    hw_int(w, u->nbiot_interval_s);
    hw_puts(w, ",\"io\":{");
    for (int i = 0; i < u->io_count; i++) {
        const io_config_t *io = &ios[i];
        if (i) hw_puts(w, ",");
        hw_str(w, io->name);
        hw_puts(w, ":{\"b\":"); hw_int(w, io->start_byte);
        hw_puts(w, io->big_endian ? ",\"e\":\"b\",\"f\":[" : ",\"e\":\"l\",\"f\":[");
        // Read codes (1-4) are always lower than write codes, so this is ascending
        if (io->read_fc) hw_int(w, io->read_fc);
        if (io->read_fc && io->write_fc) hw_puts(w, ",");
        if (io->write_fc) hw_int(w, io->write_fc);
        hw_puts(w, "],\"l\":"); hw_int(w, io->length);
        hw_puts(w, ",\"r\":");  hw_int(w, io->read_addr);
        hw_puts(w, ",\"t\":\""); hw_puts(w, VTYPE_NAMES[io->value_type]); hw_puts(w, "\"}");
    }
    hw_puts(w, "},\"n\":"); hw_str(w, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(uj, "n")));
    const char *ty = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(uj, "ty"));
    if (ty) { hw_puts(w, ",\"ty\":"); hw_str(w, ty); }
    hw_puts(w, ",\"w\":"); hw_int(w, u->wifi_interval_s);
    hw_puts(w, "}");
    return hw_finish(w);
}

static const unit_config_t *find_unit_in(const system_config_t *cfg, uint8_t key)
{
    for (int i = 0; i < cfg->unit_count; i++) {
        if (cfg->units[i].key == key) return &cfg->units[i];
    }
    return NULL;
}

// {"cu":{..},"gps":0,"id":"..","n":"..","s":2,"u":{"<key>":"<unit hash>",..}}; v is not part of it
static uint32_t hash_root(hash_writer_t *w, const cJSON *root, const system_config_t *cfg)
{
    hw_start(w);
    hw_puts(w, "{\"cu\":{");
    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(root, "cu");
    if (cu) {
        bool first = true;
        for (const cJSON *e = next_sorted(cu, NULL); e; e = next_sorted(cu, e)) {
            if (!first) hw_puts(w, ",");
            first = false;
            hw_str(w, e->string);
            hw_puts(w, ":");
            hw_int(w, (long)e->valuedouble);
        }
    }
    hw_puts(w, "},\"gps\":"); hw_int(w, cfg->gps_enabled ? 1 : 0);
    hw_puts(w, ",\"id\":");   hw_str(w, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id")));
    hw_puts(w, ",\"n\":");    hw_str(w, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "n")));
    hw_puts(w, ",\"s\":2,\"u\":{");
    // REVIEW: unit keys sorted as strings here ("10" < "2"), unlike units[] which is numeric.
    const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "u");
    bool first = true;
    for (const cJSON *uj = next_sorted(units, NULL); uj; uj = next_sorted(units, uj)) {
        uint8_t key = 0;
        parse_unit_key(uj->string, &key);
        const unit_config_t *u = find_unit_in(cfg, key);
        char hex[9];
        hash_to_hex(u ? u->hash : 0, hex);
        if (!first) hw_puts(w, ",");
        first = false;
        hw_str(w, uj->string);
        hw_puts(w, ":\"");
        hw_puts(w, hex);
        hw_puts(w, "\"");
    }
    hw_puts(w, "}}");
    return hw_finish(w);
}

// ---------------------------------------------------------------------------
// Build the in-memory config from a validated tree
// ---------------------------------------------------------------------------

static esp_err_t build_config(const cJSON *root, int io_count, int cu_count, system_config_t **out)
{
    esp_err_t ret = ESP_OK;
    hash_writer_t *w = NULL;
    system_config_t *cfg = cm_calloc(1, sizeof(system_config_t));
    ESP_GOTO_ON_FALSE(cfg, ESP_ERR_NO_MEM, fail, TAG, "No memory for config struct");
    if (io_count > 0) {
        cfg->io_pool = cm_calloc(io_count, sizeof(io_config_t));
        ESP_GOTO_ON_FALSE(cfg->io_pool, ESP_ERR_NO_MEM, fail, TAG, "No memory for %d IOs", io_count);
    }
    if (cu_count > 0) {
        cfg->configure_units = cm_calloc(cu_count, sizeof(configure_unit_t));
        ESP_GOTO_ON_FALSE(cfg->configure_units, ESP_ERR_NO_MEM, fail, TAG, "No memory for %d cu entries", cu_count);
    }

    cfg->version = (int)cJSON_GetObjectItemCaseSensitive(root, "v")->valuedouble;
    strncpy(cfg->name, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "n")), MAX_NAME_LEN - 1);
    strncpy(cfg->device_id, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id")), MAX_NAME_LEN - 1);
    const cJSON *gps = cJSON_GetObjectItemCaseSensitive(root, "gps");
    cfg->gps_enabled = gps && gps->valuedouble == 1;

    // REVIEW: insertion by numeric key so units[] is sorted and find_unit is a short loop (plan section 8).
    const cJSON *units = cJSON_GetObjectItemCaseSensitive(root, "u");
    const cJSON *uj;
    cJSON_ArrayForEach(uj, units) {
        uint8_t key = 0;
        parse_unit_key(uj->string, &key);
        int j = cfg->unit_count++;
        while (j > 0 && cfg->units[j - 1].key > key) {
            cfg->units[j] = cfg->units[j - 1];
            j--;
        }
        cfg->units[j].key = key;
    }

    w = cm_calloc(1, sizeof(hash_writer_t));
    ESP_GOTO_ON_FALSE(w, ESP_ERR_NO_MEM, fail, TAG, "No memory for hash context");
    mbedtls_md_init(&w->md);
    w->rc = mbedtls_md_setup(&w->md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);

    int pool_idx = 0;
    for (int i = 0; i < cfg->unit_count; i++) {
        unit_config_t *u = &cfg->units[i];
        char key_str[4];
        snprintf(key_str, sizeof(key_str), "%u", u->key);
        uj = cJSON_GetObjectItemCaseSensitive(units, key_str);
        u->io_pool_offset = pool_idx;
        ESP_GOTO_ON_ERROR(parse_unit(uj, u, cfg->io_pool ? &cfg->io_pool[pool_idx] : NULL, &u->io_count),
                          fail, TAG, "Unit %u failed to parse after validation", u->key);
        pool_idx += u->io_count;
        u->hash = hash_unit(w, uj, u, cfg->io_pool ? &cfg->io_pool[u->io_pool_offset] : NULL);
    }
    cfg->io_pool_count = pool_idx;

    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(root, "cu");
    if (cu) {
        for (const cJSON *e = next_sorted(cu, NULL); e; e = next_sorted(cu, e)) {
            configure_unit_t *c = &cfg->configure_units[cfg->configure_unit_count++];
            strncpy(c->sensor_type, e->string, MAX_NAME_LEN - 1);
            c->address_register = (uint16_t)e->valuedouble;
        }
    }

    cfg->hash = hash_root(w, root, cfg);
    // REVIEW: not ESP_FAIL, which is reserved for file errors ("fs"); a hash context without memory is "mem".
    ESP_GOTO_ON_FALSE(w->rc == 0, w->rc == MBEDTLS_ERR_MD_ALLOC_FAILED ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_RESPONSE,
                      fail, TAG, "SHA-256 failed: -0x%04x", (unsigned)-w->rc);

    mbedtls_md_free(&w->md);
    cm_free(w);
    *out = cfg;
    return ESP_OK;

fail:
    if (w) {
        mbedtls_md_free(&w->md);
        cm_free(w);
    }
    free_config_struct(cfg);
    return ret;
}

static void log_loaded(const system_config_t *cfg)
{
    char hex[9];
    hash_to_hex(cfg->hash, hex);
    ESP_LOGI(TAG, "Config loaded: v%d, %d units, %d IOs, hash %s. Free heap: %zu",
             cfg->version, cfg->unit_count, cfg->io_pool_count, hex, free_heap());
}

// Makes cfg the active config. Caller holds the config lock.
static void install_config(system_config_t *cfg, bool file_bad)
{
    free_config_struct(g_config);
    g_config = cfg;
    s_file_bad = file_bad;
    atomic_store(&s_config_wanted, true);
    memcpy(s_device_id, cfg->device_id, sizeof(s_device_id));
    log_loaded(cfg);
}


// REVIEW: takes ownership of root and frees it before the file write, so tree and text never sit beside a second tree.
// Caller holds the config lock. On error the active config is left as it was.
static esp_err_t commit_tree(cJSON *root)
{
    int units = 0, ios = 0, cus = 0;
    system_config_t *cfg = NULL;
    char *text = NULL;

    esp_err_t ret = validate_config(root, &units, &ios, &cus);
    if (ret == ESP_OK) ret = build_config(root, ios, cus, &cfg);
    if (ret == ESP_OK) ret = serialize_bounded(root, &text);
    cJSON_Delete(root);
    if (ret == ESP_OK) ret = write_config_file(text);
    cm_free(text);

    if (ret != ESP_OK) {
        free_config_struct(cfg);
        return ret;
    }
    install_config(cfg, false);
    return ESP_OK;
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
    memset(&s_patch_result, 0, sizeof(s_patch_result));
    s_file_bad = false;
    atomic_store(&s_config_wanted, false);
    s_device_id[0] = '\0';
    vSemaphoreDelete(s_config_mux);
    vSemaphoreDelete(s_file_mux);
    s_config_mux = NULL;
    s_file_mux = NULL;
    return ESP_OK;
}

// REVIEW: factory default per plan section 6 step 1: s, v:0, device ID, default name, no units (cu and gps left out).
static cJSON *make_default_tree(const char *device_id)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    bool ok = cJSON_AddNumberToObject(root, "s", CONFIG_MANAGER_SCHEMA_VERSION)
           && cJSON_AddNumberToObject(root, "v", 0)
           && cJSON_AddStringToObject(root, "id", device_id ? device_id : "")
           && cJSON_AddStringToObject(root, "n", DEFAULT_NAME)
           && cJSON_AddObjectToObject(root, "u");
    if (!ok) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static esp_err_t reset_locked(const char *device_id)
{
    cJSON *root = make_default_tree(device_id);
    if (!root) return ESP_ERR_NO_MEM;

    // Drop the active config now so nothing stale lingers even if the write fails
    free_config_struct(g_config);
    g_config = NULL;

    esp_err_t ret = commit_tree(root);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Factory reset complete, default config loaded (0 units)");
    else
        ESP_LOGE(TAG, "Factory reset failed (%s), config cleared; device should restart", esp_err_to_name(ret));
    return ret;
}

// REVIEW: an unreadable file is handled like a missing one (plan section 6), but only in RAM: the bad file stays until a valid config is saved.
// The device ID is taken from the bad file if it still has one ("id", or v1 "device_ID"), else known_id, else the config in RAM.
static esp_err_t fallback_locked(const cJSON *bad_root, const char *reason, const char *known_id)
{
    char device_id[MAX_NAME_LEN] = "";
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(bad_root, "id"));
    if (!id) id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(bad_root, "device_ID"));
    if (!id) id = known_id;
    if (!id && g_config) id = g_config->device_id;
    if (!id) id = s_device_id;
    // REVIEW: an ID that may not appear in a topic would make the default itself invalid, so it is dropped.
    if (id && topic_safe(id)) strncpy(device_id, id, MAX_NAME_LEN - 1);

    if (reason)
        ESP_LOGW(TAG, "%s is %s: running on the empty default config (v0, no units) until the server sends a full config; "
                 "the file is kept until then", CONFIG_MANAGER_FILE_PATH, reason);

    cJSON *root = make_default_tree(device_id);
    if (!root) return ESP_ERR_NO_MEM;
    int units = 0, ios = 0, cus = 0;
    system_config_t *cfg = NULL;
    esp_err_t ret = validate_config(root, &units, &ios, &cus);
    if (ret == ESP_OK) ret = build_config(root, ios, cus, &cfg);
    cJSON_Delete(root);
    if (ret != ESP_OK) return ret;
    install_config(cfg, true);
    return ESP_OK;
}

// Caller holds the config lock. known_id: device ID for the fallback when the RAM config was already freed.
static esp_err_t load_locked(const char *known_id)
{
    esp_err_t st = ESP_OK;
    char *text = read_config_text(NULL, &st);
    if (!text) {
        if (st == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "No config at %s, writing factory defaults", CONFIG_MANAGER_FILE_PATH);
            return reset_locked(NULL);
        }
        // REVIEW: out of memory or a lock timeout says nothing about the file, so the RAM config is left as it is.
        if (st == ESP_ERR_NO_MEM || st == ESP_ERR_TIMEOUT) return st;
        return fallback_locked(NULL, st == ESP_ERR_INVALID_SIZE ? "empty or over CONFIG_MANAGER_MAX_FILE_SIZE" : "unreadable",
                               known_id);
    }
    cJSON *root = cJSON_Parse(text);
    cm_free(text);
    if (!root) return fallback_locked(NULL, "not valid JSON", known_id);

    int units = 0, ios = 0, cus = 0;
    system_config_t *cfg = NULL;
    esp_err_t ret = validate_config(root, &units, &ios, &cus);
    if (ret == ESP_OK) ret = build_config(root, ios, cus, &cfg);
    if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE) {
        ret = fallback_locked(root, ret == ESP_ERR_INVALID_SIZE ? "over the quota" : "not a valid v2 config", known_id);
        cJSON_Delete(root);
        return ret;
    }
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not build the config from %s: %s", CONFIG_MANAGER_FILE_PATH, esp_err_to_name(ret));
        return ret;
    }
    install_config(cfg, false);
    return ESP_OK;
}

// REVIEW: called where the server talks to the library; recovers from a reload that failed after a rejected patch.
static void ensure_config_locked(void)
{
    if (g_config || !atomic_load(&s_config_wanted)) return;
    ESP_LOGW(TAG, "No config in RAM (an earlier reload failed), loading %s again", CONFIG_MANAGER_FILE_PATH);
    esp_err_t ret = load_locked(s_device_id[0] ? s_device_id : NULL);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Reload failed again (%s); retried on the next state, sync or set", esp_err_to_name(ret));
}

esp_err_t config_manager_load(void)
{
    if (!s_file_mux) return ESP_ERR_INVALID_STATE;
    // REVIEW: set before the lock so that even a lock timeout here is retried by the next state, sync or set.
    atomic_store(&s_config_wanted, true);
    if (!take_config(LOCK_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = load_locked(NULL);
    give_config();
    return ret;
}

esp_err_t config_manager_save(const char *json_data)
{
    if (!json_data) return ESP_ERR_INVALID_ARG;
    if (!s_config_mux) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        ESP_LOGE(TAG, "Rejected config: invalid JSON, file not changed");
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_config(LOCK_TIMEOUT_MS)) {
        cJSON_Delete(root);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = commit_tree(root);
    give_config();
    if (ret == ESP_OK) remove_v1_file();
    else ESP_LOGE(TAG, "Config not saved: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t config_manager_reset_to_default(const char *device_id)
{
    ESP_LOGI(TAG, "Resetting to factory defaults...");
    if (!take_config(LOCK_TIMEOUT_MS)) return s_config_mux ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    esp_err_t ret = reset_locked(device_id);
    give_config();
    return ret;
}

// ---------------------------------------------------------------------------
// Locked access
// ---------------------------------------------------------------------------

const system_config_t *config_manager_get_ptr(void)
{
    if (!take_config(1000)) return NULL;
    if (!g_config) {
        // The caller must not call config_manager_release() for NULL
        give_config();
        return NULL;
    }
    return g_config;
}

void config_manager_release(void)
{
    give_config();
}

void config_manager_unload(void)
{
    if (!take_config(LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Failed to lock config for unloading");
        return;
    }
    if (g_config) {
        free_config_struct(g_config);
        g_config = NULL;
        ESP_LOGI(TAG, "Configuration unloaded from RAM");
    }
    atomic_store(&s_config_wanted, false);   // unloading is deliberate: no automatic reload
    give_config();
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
    if (!take_config(timeout_ms)) return ESP_ERR_TIMEOUT;
    give_config();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Read helpers
// ---------------------------------------------------------------------------

int config_manager_get_version(void)
{
    if (!take_config(100)) return 0;
    int ver = g_config ? g_config->version : 0;
    give_config();
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

bool config_manager_is_fallback(void)
{
    if (!take_config(LOCK_TIMEOUT_MS)) return false;
    bool bad = s_file_bad && g_config;
    give_config();
    return bad;
}

esp_err_t config_manager_copy_string(const char *key, char *out, size_t out_size)
{
    if (!key || !out || out_size == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!s_config_mux) return ESP_ERR_INVALID_STATE;
    if (!take_config(LOCK_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_OK;
    const char *src = NULL;
    if (!g_config)                          ret = ESP_ERR_INVALID_STATE;
    else if (strcmp(key, "name") == 0)      src = g_config->name;
    else if (strcmp(key, "device_ID") == 0) src = g_config->device_id;
    else                                    ret = ESP_ERR_NOT_FOUND;
    if (src) {
        size_t len = strlen(src);
        // REVIEW: all or nothing: a value that does not fit leaves "" rather than a truncated name or ID.
        if (len >= out_size) ret = ESP_ERR_INVALID_SIZE;
        else memcpy(out, src, len + 1);
    }
    give_config();
    return ret;
}

int config_manager_find_unit(uint8_t unit_key)
{
    if (!take_config(100)) return -1;
    int idx = -1;
    if (g_config) {
        for (int i = 0; i < g_config->unit_count; i++) {
            if (g_config->units[i].key == unit_key) { idx = i; break; }
        }
    }
    give_config();
    return idx;
}

// REVIEW: get_payload's result is documented as free(); cJSON's allocator may be replaced by app hooks, so copy into cm_malloc.
static char *print_for_free(const cJSON *tree)
{
    char *tmp = cJSON_PrintUnformatted(tree);
    if (!tmp) return NULL;
    size_t len = strlen(tmp) + 1;
    char *out = cm_malloc(len);
    if (out) memcpy(out, tmp, len);
    cJSON_free(tmp);
    return out;
}

// REVIEW: while the file is unusable, readers get the default in RAM, so get/data agree with the published h.
// *bad tells the caller which one it is; returns false on a lock timeout (nothing should be read then).
static bool fallback_check(bool *bad, char device_id[MAX_NAME_LEN])
{
    if (!take_config(LOCK_TIMEOUT_MS)) return false;
    *bad = s_file_bad;
    memcpy(device_id, g_config ? g_config->device_id : s_device_id, MAX_NAME_LEN);
    give_config();
    return true;
}

// The tree get/data answers from: the default while the file is unusable, else the file.
static cJSON *read_tree(void)
{
    bool bad = false;
    char device_id[MAX_NAME_LEN];
    if (!fallback_check(&bad, device_id)) return NULL;
    return bad ? make_default_tree(device_id) : load_config_file();
}

cJSON *config_manager_get_config_json(void)
{
    return read_tree();
}

cJSON *config_manager_get_unit_json(uint8_t unit_key)
{
    cJSON *root = read_tree();
    if (!root) return NULL;
    char key_str[4];
    snprintf(key_str, sizeof(key_str), "%u", unit_key);
    // REVIEW: detached instead of duplicated so the unit is not copied while the whole tree is still allocated.
    cJSON *unit = cJSON_DetachItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "u"), key_str);
    cJSON_Delete(root);
    return unit;
}

char *config_manager_get_payload(const char *request_json)
{
    if (!request_json) return NULL;
    cJSON *req = cJSON_Parse(request_json);
    if (!cJSON_IsObject(req)) {
        ESP_LOGW(TAG, "CFG_GET: request is not a JSON object");
        cJSON_Delete(req);
        return NULL;
    }
    const cJSON *u_req  = cJSON_GetObjectItemCaseSensitive(req, "u");
    const cJSON *cu_req = cJSON_GetObjectItemCaseSensitive(req, "cu");
    if (u_req && !cJSON_IsArray(u_req)) {
        ESP_LOGW(TAG, "CFG_GET: u must be an array of unit keys");
        cJSON_Delete(req);
        return NULL;
    }

    // REVIEW: {} returns the stored file text as-is; it is already compact JSON, so no tree is built.
    if (!u_req && !cu_req) {
        cJSON_Delete(req);
        bool bad = false;
        char device_id[MAX_NAME_LEN];
        if (!fallback_check(&bad, device_id)) return NULL;
        if (!bad) return read_config_text(NULL, NULL);
        cJSON *def = make_default_tree(device_id);
        char *text = def ? print_for_free(def) : NULL;
        cJSON_Delete(def);
        return text;
    }

    bool want_cu = cJSON_IsTrue(cu_req) || (cJSON_IsNumber(cu_req) && cu_req->valuedouble != 0);
    char *out = NULL;
    cJSON *resp = NULL;
    cJSON *root = read_tree();
    if (!root) goto done;

    resp = cJSON_CreateObject();
    if (!resp) goto done;
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "v");
    bool ok = cJSON_AddNumberToObject(resp, "s", CONFIG_MANAGER_SCHEMA_VERSION)
           && cJSON_AddNumberToObject(resp, "v", cJSON_IsNumber(ver) ? ver->valuedouble : 0);

    if (ok && u_req) {
        cJSON *units_out = cJSON_AddObjectToObject(resp, "u");
        cJSON *units_file = cJSON_GetObjectItemCaseSensitive(root, "u");
        ok = units_out != NULL;
        const cJSON *k;
        cJSON_ArrayForEach(k, u_req) {
            if (!ok) break;
            if (!cJSON_IsString(k)) continue;
            // Moved out of the file tree, not copied
            cJSON *unit = cJSON_DetachItemFromObjectCaseSensitive(units_file, k->valuestring);
            if (unit) ok = cJSON_AddItemToObject(units_out, k->valuestring, unit);
            else ESP_LOGW(TAG, "CFG_GET: unit '%s' not in config", k->valuestring);
        }
    }
    if (ok && want_cu) {
        cJSON *cu = cJSON_DetachItemFromObjectCaseSensitive(root, "cu");
        if (!cu) cu = cJSON_CreateObject();
        ok = cu && cJSON_AddItemToObject(resp, "cu", cu);
    }
    cJSON_Delete(root);
    root = NULL;
    if (ok) out = print_for_free(resp);

done:
    if (!out) ESP_LOGE(TAG, "CFG_GET: could not build data payload (heap %zu)", free_heap());
    cJSON_Delete(root);
    cJSON_Delete(resp);
    cJSON_Delete(req);
    return out;
}

static void bit_set(uint8_t *map, uint8_t key) { map[key >> 3] |= (uint8_t)(1u << (key & 7)); }
static bool bit_get(const uint8_t *map, uint8_t key) { return map[key >> 3] & (1u << (key & 7)); }

cJSON *config_manager_build_state(bool all_units)
{
    if (!take_config(1000)) return NULL;
    ensure_config_locked();
    if (!g_config) {
        give_config();
        return NULL;
    }
    char hex[9];
    cJSON *state = cJSON_CreateObject();
    bool ok = state != NULL;
    hash_to_hex(g_config->hash, hex);
    ok = ok && cJSON_AddNumberToObject(state, "s", CONFIG_MANAGER_SCHEMA_VERSION)
            && cJSON_AddNumberToObject(state, "v", g_config->version)
            && cJSON_AddStringToObject(state, "h", hex);

    if (ok && all_units) {
        cJSON *u = cJSON_AddObjectToObject(state, "u");
        ok = u != NULL;
        for (int i = 0; ok && i < g_config->unit_count; i++) {
            char key[4];
            snprintf(key, sizeof(key), "%u", g_config->units[i].key);
            hash_to_hex(g_config->units[i].hash, hex);
            ok = cJSON_AddStringToObject(u, key, hex) != NULL;
        }
    } else if (ok && s_patch_result.pending && !s_patch_result.err) {
        // REVIEW: "u" only when the patch changed a unit; otherwise the state looks like the connect state (user decision).
        bool any = false;
        for (int b = 0; b < 32; b++) any = any || s_patch_result.changed[b] || s_patch_result.deleted[b];
        cJSON *u = any ? cJSON_AddObjectToObject(state, "u") : NULL;
        ok = !any || u != NULL;
        for (int k = 1; any && ok && k <= 255; k++) {
            char key[12];
            snprintf(key, sizeof(key), "%d", k);
            if (bit_get(s_patch_result.changed, (uint8_t)k)) {
                const unit_config_t *unit = find_unit_in(g_config, (uint8_t)k);
                hash_to_hex(unit ? unit->hash : 0, hex);
                ok = cJSON_AddStringToObject(u, key, hex) != NULL;
            } else if (bit_get(s_patch_result.deleted, (uint8_t)k)) {
                ok = cJSON_AddNullToObject(u, key) != NULL;
            }
        }
    }
    if (ok && s_patch_result.pending && s_patch_result.err)
        ok = cJSON_AddStringToObject(state, "err", s_patch_result.err) != NULL;

    if (ok) memset(&s_patch_result, 0, sizeof(s_patch_result));
    give_config();

    if (!ok) {
        ESP_LOGE(TAG, "CFG_STATE: out of memory (heap %zu)", free_heap());
        cJSON_Delete(state);
        return NULL;
    }
    return state;
}

cJSON *config_manager_build_limits(void)
{
    cJSON *cfg = cJSON_CreateObject();
    bool ok = cfg
           && cJSON_AddNumberToObject(cfg, "u", MAX_UNITS)
           && cJSON_AddNumberToObject(cfg, "io", MAX_IOS)
           && cJSON_AddNumberToObject(cfg, "cu", MAX_CONFIGURE_UNITS)
           && cJSON_AddNumberToObject(cfg, "b", MAX_FILE_SIZE);
    if (!ok) {
        cJSON_Delete(cfg);
        return NULL;
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Patch (v2/cfg/set) and sync
// ---------------------------------------------------------------------------

// REVIEW: unknown keys are rejected before anything is touched; values are checked on the merged result.
static bool patch_keys_ok(const cJSON *patch)
{
    if (!object_ok(patch, ROOT_KEYS, "patch")) return false;
    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(patch, "cu");
    if (cJSON_IsObject(cu) && has_duplicate_keys(cu, "patch cu")) return false;

    const cJSON *units = cJSON_GetObjectItemCaseSensitive(patch, "u");
    if (!cJSON_IsObject(units)) return true;
    if (has_duplicate_keys(units, "patch u")) return false;
    const cJSON *uj;
    cJSON_ArrayForEach(uj, units) {
        uint8_t key;
        if (!parse_unit_key(uj->string, &key)) {
            ESP_LOGW(TAG, "Rejected: unit key '%s' is not \"1\"..\"255\"", uj->string);
            return false;
        }
        if (!cJSON_IsObject(uj)) continue;
        if (!object_ok(uj, UNIT_KEYS, uj->string)) return false;
        const cJSON *ios = cJSON_GetObjectItemCaseSensitive(uj, "io");
        if (!cJSON_IsObject(ios)) continue;
        if (has_duplicate_keys(ios, "patch io")) return false;
        const cJSON *io;
        cJSON_ArrayForEach(io, ios) {
            if (cJSON_IsObject(io) && !object_ok(io, IO_KEYS, io->string)) return false;
        }
    }
    return true;
}

// RFC 7396: MergePatch({}, value) for a value that replaces a non-object
static void strip_nulls(cJSON *obj)
{
    cJSON *c = obj->child;
    while (c) {
        cJSON *next = c->next;
        if (cJSON_IsNull(c)) cJSON_Delete(cJSON_DetachItemViaPointer(obj, c));
        else if (cJSON_IsObject(c)) strip_nulls(c);
        c = next;
    }
}

// REVIEW: RFC 7396 merge that moves patch items into the target instead of copying them (no second tree).
static esp_err_t merge_patch(cJSON *target, cJSON *patch)
{
    cJSON *item;
    while ((item = patch->child) != NULL) {
        cJSON_DetachItemViaPointer(patch, item);
        cJSON *existing = cJSON_GetObjectItemCaseSensitive(target, item->string);
        if (cJSON_IsNull(item)) {
            if (existing) cJSON_Delete(cJSON_DetachItemViaPointer(target, existing));
            cJSON_Delete(item);
        } else if (cJSON_IsObject(item) && cJSON_IsObject(existing)) {
            esp_err_t ret = merge_patch(existing, item);
            cJSON_Delete(item);
            if (ret != ESP_OK) return ret;
        } else {
            if (cJSON_IsObject(item)) strip_nulls(item);
            // ReplaceItemViaPointer keeps item->string; cJSON_ReplaceItemInObject would free it before copying it
            if (existing) {
                cJSON_ReplaceItemViaPointer(target, existing, item);
            } else if (!cJSON_AddItemToObject(target, item->string, item)) {
                cJSON_Delete(item);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    return ESP_OK;
}

// What the patch needs to know about the config it replaces: unit keys, hashes and live values
typedef struct {
    int           unit_count;
    unit_config_t units[MAX_UNITS];
    float         live[];
} cfg_snapshot_t;

static cfg_snapshot_t *take_snapshot(const system_config_t *cfg)
{
    cfg_snapshot_t *snap = cm_malloc(sizeof(cfg_snapshot_t) + (size_t)cfg->io_pool_count * sizeof(float));
    if (!snap) return NULL;
    snap->unit_count = cfg->unit_count;
    memcpy(snap->units, cfg->units, sizeof(snap->units));
    for (int i = 0; i < cfg->io_pool_count; i++) snap->live[i] = cfg->io_pool[i].live_value;
    return snap;
}

// REVIEW: same key and same hash means the same IOs in the same (sorted) order, so live values carry over by position.
static void restore_live_values(system_config_t *cfg, const cfg_snapshot_t *snap)
{
    for (int i = 0; i < snap->unit_count; i++) {
        const unit_config_t *old = &snap->units[i];
        const unit_config_t *now = find_unit_in(cfg, old->key);
        if (!now || now->hash != old->hash || now->io_count != old->io_count) continue;
        for (int j = 0; j < now->io_count; j++)
            cfg->io_pool[now->io_pool_offset + j].live_value = snap->live[old->io_pool_offset + j];
    }
}

static void record_changes(const system_config_t *cfg, const cfg_snapshot_t *snap)
{
    for (int i = 0; i < cfg->unit_count; i++) {
        const unit_config_t *now = &cfg->units[i];
        const unit_config_t *old = NULL;
        for (int j = 0; j < snap->unit_count; j++) {
            if (snap->units[j].key == now->key) { old = &snap->units[j]; break; }
        }
        if (!old || old->hash != now->hash) bit_set(s_patch_result.changed, now->key);
    }
    for (int j = 0; j < snap->unit_count; j++) {
        if (!find_unit_in(cfg, snap->units[j].key)) bit_set(s_patch_result.deleted, snap->units[j].key);
    }
}

// REVIEW: short err codes decided by the user; same mapping as rule_manager: only ESP_FAIL (file read/write) is "fs".
static const char *err_text(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_SIZE:    return "q";
    case ESP_ERR_INVALID_VERSION: return "v";
    case ESP_ERR_NO_MEM:          return "mem";
    case ESP_FAIL:                return "fs";
    default:                      return "inv";
    }
}

// Caller holds the config lock. Consumes patch. *duplicate is set when v equals the stored version.
static esp_err_t apply_patch_locked(cJSON *patch, int *old_ver, bool *duplicate)
{
    esp_err_t ret = ESP_OK;
    cfg_snapshot_t *snap = NULL;
    cJSON *root = NULL;

    ESP_GOTO_ON_FALSE(cJSON_IsObject(patch), ESP_ERR_INVALID_ARG, out, TAG, "CFG_SET: body is not a JSON object");
    ESP_GOTO_ON_FALSE(g_config, ESP_ERR_INVALID_STATE, out, TAG, "CFG_SET: no config loaded");
    *old_ver = g_config->version;

    long v;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(patch, "v"), 0, INT_MAX, &v) || v < g_config->version) {
        ESP_LOGW(TAG, "CFG_SET: rejected, v missing or lower than %d", g_config->version);
        ret = ESP_ERR_INVALID_VERSION;
        goto out;
    }
    // REVIEW: same v = duplicate delivery (QoS 1 redelivery): nothing applied, no err; a different patch with a reused v is ignored too.
    if (v == g_config->version) {
        ESP_LOGI(TAG, "CFG_SET: v%ld already stored, duplicate ignored", v);
        *duplicate = true;
        goto out;
    }
    // REVIEW: a body with "s" is a complete file that replaces the stored one (user decision); no merge, file not read.
    const cJSON *s_item = cJSON_GetObjectItemCaseSensitive(patch, "s");
    bool full = s_item != NULL;
    long s_val;
    ESP_GOTO_ON_FALSE(!full || get_int(s_item, 2, 2, &s_val), ESP_ERR_INVALID_ARG, out, TAG,
                      "CFG_SET: rejected, s must be %d", CONFIG_MANAGER_SCHEMA_VERSION);
    ESP_GOTO_ON_FALSE(full || patch_keys_ok(patch), ESP_ERR_INVALID_ARG, out, TAG, "CFG_SET: rejected, unknown key");

    snap = take_snapshot(g_config);
    ESP_GOTO_ON_FALSE(snap, ESP_ERR_NO_MEM, out, TAG, "CFG_SET: no memory for snapshot");
    char device_id[MAX_NAME_LEN];
    memcpy(device_id, g_config->device_id, sizeof(device_id));

    // REVIEW: plan section 8 low-memory order: free the active config, load the file, merge, write, rebuild.
    free_config_struct(g_config);
    g_config = NULL;
    ESP_LOGD(TAG, "CFG_SET: freed active config, heap %zu", free_heap());

    if (full) {
        // REVIEW: the body itself becomes the new file; commit_tree validates it as a complete v2 file.
        root = patch;
        patch = NULL;
    } else {
        // REVIEW: with an unreadable file the patch merges onto the default in RAM, so a full config from the server repairs it.
        root = s_file_bad ? make_default_tree(device_id) : load_config_file();
        ESP_GOTO_ON_FALSE(cJSON_IsObject(root), ESP_FAIL, restore, TAG, "CFG_SET: could not read %s", CONFIG_MANAGER_FILE_PATH);
        ret = merge_patch(root, patch);
        cJSON_Delete(patch);
        patch = NULL;
        ESP_GOTO_ON_ERROR(ret, restore, TAG, "CFG_SET: merge failed");
    }

    ret = commit_tree(root);
    root = NULL;
    if (ret != ESP_OK) goto restore;

    memset(&s_patch_result, 0, sizeof(s_patch_result));
    record_changes(g_config, snap);
    restore_live_values(g_config, snap);
    goto out;

restore:
    // REVIEW: the file is untouched on every rejection, so the old config is rebuilt from it.
    cJSON_Delete(root);
    // REVIEW: while the file is unreadable the default is rebuilt in RAM with the same device ID instead of re-reading the file.
    esp_err_t reload = s_file_bad ? fallback_locked(NULL, NULL, device_id) : load_locked(device_id);
    if (reload == ESP_OK) restore_live_values(g_config, snap);
    else ESP_LOGE(TAG, "CFG_SET: could not reload the stored config");

out:
    cJSON_Delete(patch);
    cm_free(snap);
    return ret;
}

esp_err_t config_manager_apply_patch(const char *json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    if (!s_config_mux) return ESP_ERR_INVALID_STATE;

    cJSON *patch = cJSON_Parse(json);
    ESP_LOGV(TAG, "CFG_SET: %s", json);
    // REVIEW: the whole patch runs under the config lock (plan section 8); readers see NULL or time out meanwhile.
    if (!take_config(LOCK_TIMEOUT_MS)) {
        cJSON_Delete(patch);
        return ESP_ERR_TIMEOUT;
    }
    ensure_config_locked();
    int old_ver = 0;
    bool duplicate = false;
    esp_err_t ret = apply_patch_locked(patch, &old_ver, &duplicate);

    // REVIEW: a lower or missing v is reported with err "v"; a duplicate leaves any unreported patch result in place.
    if (ret == ESP_OK) {
        if (!duplicate) s_patch_result.pending = true;
    } else if (ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_TIMEOUT) {
        // REVIEW: a lock timeout records nothing, as in rule_manager.
        memset(&s_patch_result, 0, sizeof(s_patch_result));
        s_patch_result.pending = true;
        s_patch_result.err = err_text(ret);
    }
    int new_ver = g_config ? g_config->version : 0;
    give_config();

    if (duplicate) return ESP_OK;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "CFG_SET: v%d -> v%d applied", old_ver, new_ver);
        remove_v1_file();
    } else {
        ESP_LOGW(TAG, "CFG_SET: rejected (%s), config v%d kept", esp_err_to_name(ret), new_ver);
    }
    return ret;
}

bool config_manager_handle_sync(const char *json_data)
{
    if (!json_data) return false;
    cJSON *msg = cJSON_Parse(json_data);
    const char *server_h = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "h"));
    if (!server_h) {
        ESP_LOGW(TAG, "CFG_SYNC: body has no \"h\" string, ignored");
        cJSON_Delete(msg);
        return false;
    }
    if (!take_config(1000)) {
        cJSON_Delete(msg);
        return false;
    }
    ensure_config_locked();
    bool out_of_sync = false;
    if (g_config) {
        char hex[9];
        hash_to_hex(g_config->hash, hex);
        out_of_sync = strcmp(hex, server_h) != 0;
        ESP_LOGI(TAG, "CFG_SYNC: local %s, server %s: %s", hex, server_h, out_of_sync ? "out of sync" : "in sync");
    } else {
        ESP_LOGE(TAG, "CFG_SYNC: config not loaded");
    }
    give_config();
    cJSON_Delete(msg);
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
    if (!take_config(100)) return ESP_ERR_TIMEOUT;

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
    give_config();
    return ret;
}

// REVIEW: lookup by unit key and access in one lock hold, so a patch cannot move the unit in between.
static esp_err_t live_by_key_locked(uint8_t unit_key, const char *io_name, io_config_t **io_out)
{
    if (!g_config) return ESP_ERR_INVALID_STATE;
    const unit_config_t *unit = find_unit_in(g_config, unit_key);
    if (!unit) return ESP_ERR_NOT_FOUND;
    for (int i = 0; i < unit->io_count; i++) {
        io_config_t *io = &g_config->io_pool[unit->io_pool_offset + i];
        if (strcmp(io->name, io_name) == 0) {
            *io_out = io;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t config_manager_set_live_value(uint8_t unit_key, const char *io_name, float value)
{
    if (!io_name) return ESP_ERR_INVALID_ARG;
    if (!s_config_mux) return ESP_ERR_INVALID_STATE;
    if (!take_config(100)) return ESP_ERR_TIMEOUT;
    io_config_t *io = NULL;
    esp_err_t ret = live_by_key_locked(unit_key, io_name, &io);
    if (ret == ESP_OK) io->live_value = value;
    give_config();
    return ret;
}

esp_err_t config_manager_get_live_value(uint8_t unit_key, const char *io_name, float *out)
{
    if (!io_name || !out) return ESP_ERR_INVALID_ARG;
    *out = NAN;
    if (!s_config_mux) return ESP_ERR_INVALID_STATE;
    if (!take_config(100)) return ESP_ERR_TIMEOUT;
    io_config_t *io = NULL;
    esp_err_t ret = live_by_key_locked(unit_key, io_name, &io);
    if (ret == ESP_OK) *out = io->live_value;
    give_config();
    return ret;
}

float config_get_live_value(const char *io_name, uint8_t unit_idx)
{
    if (!io_name) return NAN;
    if (!take_config(100)) return NAN;
    const io_config_t *io = find_io_locked(io_name, unit_idx);
    float v = io ? io->live_value : NAN;
    give_config();
    return v;
}
