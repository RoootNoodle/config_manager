# API

Header: `config_manager.h`. The names and types are the same as the in-app `components/config_manager` they replace.

## Locking

There are two recursive mutexes: one for the active config (`g_config` plus the live values in `io_pool`) and one for the config file.

| Function | |
|---|---|
| `config_manager_get_ptr()` / `config_manager_release()` | Locked pointer to `system_config_t`. **When it returns `NULL` it holds no lock, so do not release.** |
| `config_manager_lock()` / `config_manager_unlock()` | Lock for a multi-step transaction (no timeout). |
| `config_manager_await_idle(ms)` | Waits until no other task holds the config lock, e.g. a Modbus scan before an update. |
| `config_manager_get_string("name" \| "device_ID")` | Returns a pointer into the active config. It stays valid only until the next load or update. |

Partial updates and unit deletes free the active config before merging, to save heap. Readers must handle `get_ptr()` returning `NULL`, and live-value calls return NaN or an error while that happens.

## Lifecycle

| Function | |
|---|---|
| `config_manager_init()` | Creates the mutexes. Safe to call again. |
| `config_manager_deinit()` | Frees the config and deletes the mutexes. |
| `config_manager_load()` | Reads `CONFIG_CONFIG_MANAGER_FILE_PATH`. If the file is missing, factory defaults are written and loaded. |
| `config_manager_save(json)` | Parses first, then writes and activates. **Invalid JSON returns `ESP_FAIL` and the file is not touched.** |
| `config_manager_reset_to_default(device_id)` | Writes and loads `{config_version:1, device_ID, name:"MIOT-Gateway", gps_enabled:false, units:{}, configure_units:{MIOT_Level_Sensor:298, MIOT_Trigger:299}}`. |
| `config_manager_unload()` | Frees the active config. The file is kept. |

## Read helpers

| Function | Returns |
|---|---|
| `config_manager_get_version()` | `config_version`, or 0 if no config is loaded |
| `config_manager_get_all_unit_hashes()` | `cJSON*` `{"unit_1":"<sha256>",...}`. Free it with `cJSON_Delete`. |
| `config_manager_compute_configure_units_hash(buf65)` | The cached configure_units hash |
| `config_manager_get_config_json()` | The whole file as `cJSON*`, including pass-through fields. Free it with `cJSON_Delete`. |
| `config_manager_get_unit_json(key)` | One unit from the file. Free it with `cJSON_Delete`. |
| `config_manager_get_units_payload(req)` | For `{"units":["unit_1",...],"configure_units":true}`, returns a string `{"config_version":N,"units":{...},"configure_units":{...}}`. Free it with `free()`. |

## Update handlers (MQTT)

`config_manager_handle_partial_update(json)` returns `true` if the update was applied.
- It is ignored unless `config_version` is greater than the current version.
- `name`, `gps_enabled` and `device_ID` replace the root values.
- `delete_units: [...]` removes units.
- Every unit listed in `units: {...}` replaces the existing unit of the same key, or is added.
- `configure_units` replaces the whole object.
- Every other root field (e.g. `MQTT_URL`) is kept from the file.

`config_manager_handle_sync(json)` returns `true` if the device is out of sync.
- It takes `{config_version, units_hash:{key:hash}, configure_units_hash, delete_units?}`.
- It reports a mismatch on any of: a version difference, a different unit hash, a missing unit, an extra unit, or a different configure_units hash.
- Any `delete_units` are applied, and also count as out of sync.

`config_manager_delete_unit(key)` returns `false` if the unit doesn't exist.

## Live values

| Function | |
|---|---|
| `config_update_live_value(io_name, unit_idx, value)` | `ESP_ERR_INVALID_ARG` (no config or bad unit index), `ESP_ERR_NOT_FOUND` (no such IO), `ESP_ERR_TIMEOUT` (lock not taken within 100 ms) |
| `config_get_live_value(io_name, unit_idx)` | `NaN` until first written, on a miss, or on a lock timeout |

`unit_idx` is the position in `units[]`. Units are sorted by key on load, so `unit_1` comes before `unit_2`. Live values are reset to NaN by every load and update.
