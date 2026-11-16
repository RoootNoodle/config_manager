# API

Header: `config_manager.h`. Schema v2; the MQTT messages are described in [V2_PLAN.md](V2_PLAN.md). The library builds and parses the message bodies; the application owns the MQTT client and the topic strings (`devices/{id}/v2/cfg/*`, `devices/{id}/v2/caps`).

## Locking

There are two recursive mutexes: one for the active config (`g_config`, the live values in `io_pool`, and the result of the last patch) and one for the config file. The config lock is always taken before the file lock.

| Function | |
|---|---|
| `config_manager_get_ptr()` / `config_manager_release()` | Locked pointer to `system_config_t`. **When it returns `NULL` it holds no lock, so do not release.** |
| `config_manager_lock()` / `config_manager_unlock()` | Lock for a multi-step transaction (no timeout). |
| `config_manager_await_idle(ms)` | Waits until no other task holds the config lock, e.g. a Modbus scan before an update. |
| `config_manager_get_string("name" \| "device_ID")` | Returns a pointer into the active config. It stays valid only until the next load or update. |

`config_manager_apply_patch()`, `_save()`, `_load()` and `_reset_to_default()` hold the config lock for their whole run, including the file write. A patch frees the active config before it reads the file, to save heap. Other tasks see `get_ptr()` time out or return `NULL` meanwhile, and live-value calls return NaN or an error.

## Lifecycle

| Function | |
|---|---|
| `config_manager_init()` | Creates the mutexes. Safe to call again. |
| `config_manager_deinit()` | Frees the config and deletes the mutexes. |
| `config_manager_load()` | Reads `CONFIG_CONFIG_MANAGER_FILE_PATH`. A missing file is replaced by the factory default (written). A file that cannot be used (not JSON, empty, unreadable, not valid v2, over the quota) gives the default **in RAM only** with a warning in the log, and returns `ESP_OK`; the bad file is kept until a valid config is saved. Device ID of that default: the `id` of the bad file if it is still readable JSON (`id`, or `device_ID` of a v1 file), otherwise the device ID of the config that was in RAM, otherwise `""`. `ESP_ERR_NO_MEM` or `ESP_ERR_TIMEOUT` while reading leave the RAM config as it was. |
| `config_manager_save(json)` | Validates a complete v2 file, writes it as compact JSON and activates it. Rejected input leaves the file untouched (`ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_SIZE` for quota). On success the v1 file is deleted. |
| `config_manager_reset_to_default(device_id)` | Writes and loads `{"s":2,"v":0,"id":device_id,"n":"MIOT-Gateway","u":{}}`. Does not delete the v1 file. |
| `config_manager_unload()` | Frees the active config. The file is kept. |

## Read helpers

| Function | Returns |
|---|---|
| `config_manager_get_version()` | `v`, or 0 if no config is loaded |
| `config_manager_find_unit(key)` | Index into `units[]` for unit key 1–255, or -1 |
| `config_manager_get_config_json()` | The whole file as `cJSON*`. Free it with `cJSON_Delete`. |
| `config_manager_get_unit_json(key)` | One unit from the file. Free it with `cJSON_Delete`. |

The unit name (`n`) and type (`ty`) are not kept in RAM; read them with `config_manager_get_unit_json()`.

## MQTT bodies

| Topic | Call | Notes |
|---|---|---|
| `v2/cfg/set` (in) | `config_manager_apply_patch(body)` | Then publish `config_manager_build_state(false)` |
| `v2/cfg/sync` (in) | `config_manager_handle_sync(body)` | `true`: publish `config_manager_build_state(true)`; `false`: stay silent |
| `v2/cfg/get` (in) | `config_manager_get_payload(body)` | Publish the string on `v2/cfg/data`, then `free()` it |
| `v2/cfg/state` (out) | `config_manager_build_state(all_units)` | `cJSON_PrintUnformatted`, then `cJSON_Delete` |
| `v2/caps` (out) | `config_manager_build_limits()` | The `cfg` object; the application adds `fw`, `hw`, `s`, `msg` and `rl` |

### `config_manager_apply_patch(json)`

JSON Merge Patch (RFC 7396) on the stored file: objects merge key by key, other values (arrays too) replace, `null` deletes.

1. `v` is required. Lower or missing: `ESP_ERR_INVALID_VERSION` (`"err":"v"`). **Equal to the stored `v`: a duplicate delivery; nothing is applied, `ESP_OK` is returned and the next state is the plain state without `err`.** A different patch that reuses the stored `v` is silently ignored the same way; Web sees it only as a hash mismatch.
2. Unknown keys anywhere in the patch: `ESP_ERR_INVALID_ARG`, before anything is touched.
3. The active config is freed, the file is read and merged with the patch.
4. The result is validated as a complete v2 file and checked against the quotas (`ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_SIZE`). On any rejection the file is not touched and the old config is loaded from it again, with its live values.
5. The file is written, the config rebuilt and the hashes recomputed. Live values of units whose hash did not change are kept; changed units start at NaN. The v1 file is deleted.

Other results: `ESP_ERR_NO_MEM`, `ESP_FAIL` (file read or write failed), `ESP_ERR_INVALID_STATE` (not initialised, or `config_manager_load()` never succeeded), `ESP_ERR_TIMEOUT`.

While `config_manager_load()` is running on the default for an unreadable file, a patch is merged onto that default instead of the file, so the server's full config (`v` above 0) is accepted and written, and the bad file is replaced. A rejected patch keeps the default and the bad file.

### `config_manager_build_state(all_units)`

`{"s":2,"v":9,"h":"a1b2c3d4"}` plus:

| When | Extra |
|---|---|
| `all_units == true` (after a sync mismatch) | `"u"` with every unit hash |
| First call after a successful patch | `"u"` with the units the patch changed, `null` for a deleted one; left out if no unit changed |
| First call after a rejected patch | `"err"`; `v` and `h` describe the unchanged config |
| Otherwise (on connect, after a duplicate `v`) | nothing |

The patch result is reported by the next call only, so call it right after `config_manager_apply_patch()`. `err` codes: `"q"` quota, `"v"` lower or missing `v`, `"inv"` invalid (bad JSON, unknown key, invalid result), `"mem"` out of memory, `"fs"` file read or write failed. A duplicate `v` records nothing and leaves a patch result that was not yet reported in place, so a redelivery before `build_state()` still reports the first delivery's changed units. Returns NULL when no config is loaded.

### `config_manager_handle_sync(json)`

Takes `{"v":9,"h":"a1b2c3d4"}` and compares `h` with the root hash. `v` is not compared. Returns `false` for a malformed body (no `"h"` string) or when no config is loaded.

### `config_manager_get_payload(json)`

| Request | Answer |
|---|---|
| `{}` | the stored file as-is |
| `{"u":["2","3"]}` | `{"s":2,"v":9,"u":{"2":{…},"3":{…}}}`; units that do not exist are left out |
| `{"cu":1}` | `{"s":2,"v":9,"cu":{…}}` |
| both | both |

Returns NULL when the request is not a JSON object, `u` is not an array, or the file cannot be read. Free the result with `free()`.

### `config_manager_build_limits()`

`{"u":MAX_UNITS,"io":MAX_IOS,"cu":MAX_CONFIGURE_UNITS,"b":MAX_FILE_SIZE}` from Kconfig.

## Live values

| Function | |
|---|---|
| `config_update_live_value(io_name, unit_idx, value)` | `ESP_ERR_INVALID_ARG` (no config or bad unit index), `ESP_ERR_NOT_FOUND` (no such IO), `ESP_ERR_TIMEOUT` (lock not taken within 100 ms) |
| `config_get_live_value(io_name, unit_idx)` | `NaN` until first written, on a miss, or on a lock timeout |

`unit_idx` is the position in `units[]`; get it with `config_manager_find_unit(key)`. An index is only valid until the next load, save or patch. A load or save resets every live value to NaN; a patch resets only those of the units it changed.
