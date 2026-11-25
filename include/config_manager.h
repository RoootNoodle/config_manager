#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MANAGER_SCHEMA_VERSION 2

#define MAX_UNITS           CONFIG_CONFIG_MANAGER_MAX_UNITS
#define MAX_CONFIGURE_UNITS CONFIG_CONFIG_MANAGER_MAX_CONFIGURE_UNITS
#define MAX_NAME_LEN        40
#define MAX_HASH_LEN        65

typedef enum {
    IO_VTYPE_UINT16 = 0,
    IO_VTYPE_INT16,
    IO_VTYPE_UINT32,
    IO_VTYPE_INT32,
    IO_VTYPE_FP32,
    IO_VTYPE_BOOL,
    IO_VTYPE_FP64,
} io_value_type_t;

// Byte order of a multi-byte value; A is the most significant byte. Names are for 32 bits, 64-bit values follow the same rule per word.
typedef enum {
    IO_ORDER_ABCD = 0, // "b": big endian
    IO_ORDER_DCBA,     // "l": all bytes reversed
    IO_ORDER_CDAB,     // "ws": word order reversed, bytes in each word big endian
    IO_ORDER_BADC,     // "bs": bytes swapped in each word, word order kept
} io_byte_order_t;

typedef struct {
    char            name[MAX_NAME_LEN];
    uint16_t        read_addr;
    uint8_t         read_fc;      // 0 = not readable; else 1-4
    uint8_t         write_fc;     // 0 = not writable; else 5, 6, 15 or 16
    uint8_t         start_byte;
    uint8_t         length;
    io_value_type_t value_type;
    io_byte_order_t byte_order;
    float           live_value;   // NAN until first read
} io_config_t;

typedef struct {
    char     sensor_type[MAX_NAME_LEN];
    uint16_t address_register;
} configure_unit_t;

typedef struct {
    uint8_t  key;                 // the unit key, "1"-"255" in the file
    uint8_t  address;
    uint16_t wifi_interval_s;
    uint16_t nbiot_interval_s;
    uint32_t hash;                // first 4 bytes of the SHA-256, printed as 8 hex characters
    int      io_count;
    int      io_pool_offset;      // index into system_config_t.io_pool
} unit_config_t;

typedef struct {
    int      version;
    char     name[MAX_NAME_LEN];
    char     device_id[MAX_NAME_LEN];
    uint32_t hash;                // root hash; covers cu, so there is no separate configure_units hash
    bool     gps_enabled;

    unit_config_t units[MAX_UNITS];   // sorted by key
    int           unit_count;

    io_config_t  *io_pool;        // all IOs of all units, contiguous; per unit sorted by name
    int           io_pool_count;

    configure_unit_t *configure_units;
    int               configure_unit_count;
} system_config_t;

// Lifecycle
esp_err_t config_manager_init(void);
esp_err_t config_manager_deinit(void);
esp_err_t config_manager_load(void);

/**
 * @brief Validate, write and activate a complete v2 config file.
 *
 * The file is not touched when the JSON is rejected. On success the v1 file
 * (CONFIG_CONFIG_MANAGER_V1_FILE_PATH) is deleted.
 *
 * @param json_data Complete v2 config (any formatting; stored as compact JSON).
 * @return
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_ARG if json_data is NULL, not JSON, has an unknown key or is not a valid v2 file
 *  - ESP_ERR_INVALID_SIZE if it goes over a quota (units, IOs, cu entries, file size)
 *  - ESP_ERR_NO_MEM if an allocation failed
 *  - ESP_ERR_INVALID_STATE if config_manager_init() was not called
 *  - ESP_ERR_TIMEOUT if the config or file lock was not taken
 *  - ESP_FAIL if the file could not be written
 */
esp_err_t config_manager_save(const char *json_data);
esp_err_t config_manager_reset_to_default(const char *device_id);

// Locked access; get_ptr() returns NULL (and holds no lock) when no config is loaded
const system_config_t *config_manager_get_ptr(void);
void      config_manager_release(void);
void      config_manager_unload(void);
void      config_manager_lock(void);
void      config_manager_unlock(void);
esp_err_t config_manager_await_idle(uint32_t timeout_ms);

// Read helpers
int         config_manager_get_version(void);
cJSON      *config_manager_get_config_json(void);          // from file (or the fallback default); caller cJSON_Delete()

/**
 * @brief Pointer to a string of the active config.
 *
 * The pointer points into the active config, which a load, save or patch frees. The caller must hold
 * config_manager_lock() while calling this and while using the result. New code should use
 * config_manager_copy_string().
 *
 * @param key "name" or "device_ID".
 * @return The string, or NULL for an unknown key or when no config is loaded.
 */
const char *config_manager_get_string(const char *key);

/**
 * @brief Copy a string of the active config under the config lock.
 *
 * @param key      "name" or "device_ID" (same keys as config_manager_get_string()).
 * @param out      Destination; on any error other than ESP_ERR_INVALID_ARG it holds "".
 * @param out_size Size of out in bytes, including the terminating NUL.
 * @return
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_ARG if key or out is NULL or out_size is 0
 *  - ESP_ERR_NOT_FOUND if the key is unknown
 *  - ESP_ERR_INVALID_SIZE if the string does not fit (out is "")
 *  - ESP_ERR_INVALID_STATE if no config is loaded
 *  - ESP_ERR_TIMEOUT if the config lock was not taken
 */
esp_err_t config_manager_copy_string(const char *key, char *out, size_t out_size);

/**
 * @brief Whether the active config is the in-RAM default standing in for an unusable stored file.
 *
 * While this is true the bad file is kept on flash until the server's config replaces it, so the
 * application must not call config_manager_reset_to_default() (that would overwrite it).
 *
 * @return true while running on the fallback; false otherwise, or if the config lock was not taken.
 */
bool config_manager_is_fallback(void);

/**
 * @brief Index of a unit in system_config_t.units[].
 *
 * @param unit_key Unit key (1-255).
 * @return Index into units[], or -1 if the unit does not exist, no config is loaded or the lock timed out.
 */
int config_manager_find_unit(uint8_t unit_key);

/**
 * @brief One unit object as stored in the file.
 *
 * @param unit_key Unit key (1-255).
 * @return The unit (caller cJSON_Delete()), or NULL if it does not exist or the file cannot be read.
 */
cJSON *config_manager_get_unit_json(uint8_t unit_key);

/**
 * @brief Build the body of a v2/cfg/data message for a v2/cfg/get request.
 *
 * `{}` returns the whole config file. `{"u":["2","3"]}` and/or `{"cu":1}` return
 * `{"s":2,"v":N,"u":{...},"cu":{...}}`; units that do not exist are left out.
 *
 * @param request_json Body of the get message.
 * @return Compact JSON string (caller free()), or NULL if the request is malformed or the file cannot be read.
 */
char *config_manager_get_payload(const char *request_json);

/**
 * @brief Build the body of a v2/cfg/state message.
 *
 * After config_manager_apply_patch() the next call reports that patch: the changed
 * units (null for a deleted one; `u` left out when no unit changed), or `err` if it was
 * rejected (`q`, `v`, `inv`, `mem`, `fs`). That result is reported once.
 *
 * @param all_units true after a sync mismatch: list every unit hash.
 * @return `{"s":2,"v":N,"h":"xxxxxxxx",...}` (caller cJSON_Delete()), or NULL if no config is loaded or out of memory.
 */
cJSON *config_manager_build_state(bool all_units);

/**
 * @brief Build the `cfg` object of the v2/caps message from Kconfig.
 *
 * @return `{"u":..,"io":..,"cu":..,"b":..}` (caller cJSON_Delete()), or NULL if out of memory.
 */
cJSON *config_manager_build_limits(void);

/**
 * @brief Apply a v2/cfg/set body to the stored config.
 *
 * Two forms:
 *  - Without the root key `s`: a JSON Merge Patch (RFC 7396) on the stored file.
 *  - With `s` (must be 2): a complete config file that replaces the stored one; the stored file is not read
 *    and nothing is merged. Units it does not contain are deleted. This is also how a config is repaired
 *    while running on the fallback.
 *
 * The result must be a valid v2 file within quota, otherwise the stored config is kept.
 * Live values of units whose hash did not change are kept. Publish config_manager_build_state(false) afterwards.
 *
 * A `v` equal to the stored version is a duplicate delivery: nothing is applied, ESP_OK is returned
 * and nothing is recorded: the next state is the plain state without `err`, or still the result of
 * an earlier patch that was not reported yet. A different patch reusing that `v` is ignored
 * the same way; the server sees it only as a hash mismatch.
 *
 * @param json Patch body; `v` is required and must be higher than the stored version to be applied.
 * @return
 *  - ESP_OK on success, or when `v` equals the stored version (duplicate, nothing applied)
 *  - ESP_ERR_INVALID_VERSION if `v` is missing or lower than the stored version (state `err` is "v")
 *  - ESP_ERR_INVALID_ARG if json is NULL, not a JSON object, has an unknown key, `s` is not 2, or the result is
 *    not a valid v2 file
 *  - ESP_ERR_INVALID_SIZE if the result goes over a quota (state `err` is "q")
 *  - ESP_ERR_NO_MEM if an allocation failed
 *  - ESP_ERR_INVALID_STATE if config_manager_init() was not called, config_manager_load() was never called,
 *    after config_manager_unload(), or the config cannot be loaded right now (it is retried on every call;
 *    an unreadable file does not cause this: the patch is merged onto the default config)
 *  - ESP_ERR_TIMEOUT if the config or file lock was not taken
 *  - ESP_FAIL if the file could not be read or written
 */
esp_err_t config_manager_apply_patch(const char *json);

/**
 * @brief Compare a v2/cfg/sync body `{"v":N,"h":"xxxxxxxx"}` with the root hash.
 *
 * @param json_data Body of the sync message.
 * @return true if the hash differs (publish config_manager_build_state(true)); false if it matches,
 *         the message is malformed or no config is loaded.
 */
bool config_manager_handle_sync(const char *json_data);

/**
 * @brief Store the live value of an IO, looked up by unit key under one lock hold.
 *
 * @param unit_key Unit key (1-255).
 * @param io_name  IO name.
 * @param value    New value.
 * @return
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_ARG if io_name is NULL
 *  - ESP_ERR_NOT_FOUND if the unit or the IO does not exist
 *  - ESP_ERR_INVALID_STATE if not initialised or no config is loaded
 *  - ESP_ERR_TIMEOUT if the config lock was not taken within 100 ms
 */
esp_err_t config_manager_set_live_value(uint8_t unit_key, const char *io_name, float value);

/**
 * @brief Read the live value of an IO, looked up by unit key under one lock hold.
 *
 * @param unit_key Unit key (1-255).
 * @param io_name  IO name.
 * @param out      Receives the value (NAN until first written; NAN on any error).
 * @return
 *  - ESP_OK on success
 *  - ESP_ERR_INVALID_ARG if io_name or out is NULL
 *  - ESP_ERR_NOT_FOUND if the unit or the IO does not exist
 *  - ESP_ERR_INVALID_STATE if not initialised or no config is loaded
 *  - ESP_ERR_TIMEOUT if the config lock was not taken within 100 ms
 */
esp_err_t config_manager_get_live_value(uint8_t unit_key, const char *io_name, float *out);

// Live values by index into units[]; only valid while the caller holds config_manager_lock() across
// config_manager_find_unit() and this call. New code should use the key-based functions above.
esp_err_t config_update_live_value(const char *io_name, uint8_t unit_idx, float new_val);
float     config_get_live_value(const char *io_name, uint8_t unit_idx);  // NAN on miss

#ifdef __cplusplus
}
#endif
