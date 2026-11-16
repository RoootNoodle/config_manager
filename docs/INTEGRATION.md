# Integration

## Kconfig per chip

Everything is in `menuconfig` under **Config Manager Configuration**.

| Setup | Settings | Result |
|---|---|---|
| ESP32-C6, no PSRAM (MIOT_GW) | defaults | Plain `malloc` from internal RAM; 15 units, 120 IOs, 5800 B file |
| ESP32-C5 + PSRAM (MIOT_GW_NTN) | `CONFIG_MANAGER_MAX_IOS=600`, `CONFIG_MANAGER_MAX_FILE_SIZE=28000` | Config and JSON buffers prefer PSRAM and fall back to internal RAM. This replaces NTN's hand-written `heap_caps_malloc(SPIRAM)` calls. |
| Any chip, must stay internal | `CONFIG_MANAGER_ALLOC_INTERNAL=y` | |

Options:

| Setting | Default | |
|---|---|---|
| `CONFIG_MANAGER_FILE_PATH` | `/spiffs/config_v2.json` | the v2 config file |
| `CONFIG_MANAGER_V1_FILE_PATH` | `/spiffs/config.json` | deleted after the first v2 save or patch; empty disables |
| `CONFIG_MANAGER_MAX_UNITS` | 15 | quota, also sizes `units[]` |
| `CONFIG_MANAGER_MAX_CONFIGURE_UNITS` | 20 | quota for `cu` |
| `CONFIG_MANAGER_MAX_IOS` | 120 | quota, all units together |
| `CONFIG_MANAGER_MAX_FILE_SIZE` | 5800 | quota in bytes of compact JSON; each save or patch allocates a buffer of this size |

In C, all of these get the usual `CONFIG_` prefix (e.g. `CONFIG_CONFIG_MANAGER_FILE_PATH`). Put them in the application's `sdkconfig.defaults`.

**RAM.** The peak of a load or patch is roughly the file text plus the cJSON tree, about 8.5 times the file size (estimated, V2_PLAN.md section 7): about 48 kB at 5800 B on the C6. Confirm it with a heap log on the device. What stays in RAM afterwards is `system_config_t` plus one `io_config_t` (about 60 B) per IO.

cJSON trees use the app's cJSON hooks if any are installed, as with NTN/Node's `init_cjson_psram_hooks()`.

## Moving MIOT_GW* from 1.x to 2.0

The firmware changes are listed in V2_PLAN.md section 10. Per call site:

| v1 | v2 |
|---|---|
| `version: v1.0.0` in `main/idf_component.yml` | `version: v2.0.0` |
| `config_manager_handle_partial_update(json)` (`cfg/update`, BLE, auto-addressing) | `config_manager_apply_patch(json)` on `v2/cfg/set`, then publish `config_manager_build_state(false)` on `v2/cfg/state`. It returns `esp_err_t`, not `bool`. |
| `config_manager_delete_unit(key)` | a patch `{"v":N,"u":{"3":null}}` |
| `config_manager_handle_sync(json)` returning `true` -> `cfg/hello` | same call on `v2/cfg/sync`; on `true` publish `config_manager_build_state(true)` |
| `config_manager_get_all_unit_hashes()`, `_compute_configure_units_hash()` in the hello | `config_manager_build_state(false)` on connect |
| `config_manager_get_units_payload(req)` on `cfg/request` | `config_manager_get_payload(req)` on `v2/cfg/get`, answer on `v2/cfg/data` |
| — | `config_manager_build_limits()` as `cfg` in `v2/caps`, once after every connect |
| `units[i].name` matched against a topic or rule | `config_manager_find_unit(key)`; `units[i].key` is the numeric key. `name` is removed so such code fails to compile. |
| `units[i].hash` (string) | `uint32_t`; print with `"%08" PRIx32` |
| `system_hash`, `configure_units_hash` | `hash` (root hash, covers `cu`) |
| `config_get_gps_enabled()` NVS mirror (NTN) | `system_config_t.gps_enabled` |
| `config_manager_get_unit_json("unit_2")` | `config_manager_get_unit_json(2)` |

**Device ID.** `config_manager_load()` has no device-ID parameter. When it writes the factory default (no file), the ID is `""`; when it falls back to the default for an unreadable file, the ID is the `id` of the bad file if it is still readable JSON (`id`, or `device_ID` of a v1 file), otherwise the device ID of the config that was in RAM, otherwise `""`. To put the real ID in, call `config_manager_reset_to_default(id)` after `config_manager_load()` when `config_manager_get_version()` is 0 and `config_manager_get_string("device_ID")` differs from the ID. Note that this writes the default to the file, so an unreadable file is replaced at that point. Leaving the ID as it is also works: the server's full config carries `id`, and a wrong ID only changes the root hash, which already differs at `v` 0.

Auto-addressing that adds a unit itself must pick the lowest key never used on the gateway, not `count + 1` (V2_PLAN.md section 10).

## Removing the component

Delete the dependency and the calls. Any code reading `system_config_t` (Modbus, rule adapter, BLE) goes with it.
