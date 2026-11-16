#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MANAGER_SCHEMA_VERSION 1

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
} io_value_type_t;

typedef struct {
    char            name[MAX_NAME_LEN];
    uint16_t        read_addr;
    uint8_t         read_fc;      // 0 = not readable; else 1-4
    uint8_t         write_fc;     // 0 = not writable; else 5, 6, 15 or 16
    uint8_t         start_byte;
    uint8_t         length;
    io_value_type_t value_type;
    bool            big_endian;
    float           live_value;   // NAN until first read
} io_config_t;

typedef struct {
    char     sensor_type[MAX_NAME_LEN];
    uint16_t address_register;
} configure_unit_t;

typedef struct {
    char     name[MAX_NAME_LEN];  // the JSON key, e.g. "unit_1"
    uint8_t  address;
    uint16_t wifi_interval_s;
    uint16_t nbiot_interval_s;
    char     hash[MAX_HASH_LEN];
    int      io_count;
    int      io_pool_offset;      // index into system_config_t.io_pool
} unit_config_t;

typedef struct {
    int   version;
    char  name[MAX_NAME_LEN];
    char  device_id[MAX_NAME_LEN];
    char  system_hash[MAX_HASH_LEN];
    char  configure_units_hash[MAX_HASH_LEN];

    unit_config_t units[MAX_UNITS];
    int           unit_count;

    io_config_t  *io_pool;        // all IOs of all units, contiguous
    int           io_pool_count;

    configure_unit_t *configure_units;
    int               configure_unit_count;
} system_config_t;

// Lifecycle
esp_err_t config_manager_init(void);
esp_err_t config_manager_deinit(void);
esp_err_t config_manager_load(void);                    // missing file -> factory defaults written
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
const char *config_manager_get_string(const char *key);   // "name" or "device_ID"
cJSON      *config_manager_get_all_unit_hashes(void);      // caller cJSON_Delete()
esp_err_t   config_manager_compute_configure_units_hash(char *hash_out);  // 65-byte buffer
cJSON      *config_manager_get_config_json(void);          // from file; caller cJSON_Delete()
cJSON      *config_manager_get_unit_json(const char *unit_key);  // caller cJSON_Delete()
char       *config_manager_get_units_payload(const char *request_json);  // caller free()

// Update handlers
bool config_manager_handle_partial_update(const char *json_data);
bool config_manager_handle_sync(const char *json_data);   // true = out of sync
bool config_manager_delete_unit(const char *unit_key);

// Live values (stored in io_pool, protected by the config lock)
esp_err_t config_update_live_value(const char *io_name, uint8_t unit_idx, float new_val);
float     config_get_live_value(const char *io_name, uint8_t unit_idx);  // NAN on miss

#ifdef __cplusplus
}
#endif
