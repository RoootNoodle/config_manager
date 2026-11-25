# Integration

## Kconfig per chip

Everything is in `menuconfig` under **Config Manager Configuration**.

| Setup | Settings | Result |
|---|---|---|
| ESP32-C6, no PSRAM (MIOT_GW) | defaults | Plain `malloc` from internal RAM; 15 units, 100 IOs, 4800 B file |
| ESP32-C5 + PSRAM (MIOT_GW_NTN) | `CONFIG_MANAGER_MAX_IOS=600`, `CONFIG_MANAGER_MAX_FILE_SIZE=28000` | Config and JSON buffers prefer PSRAM and fall back to internal RAM. This replaces NTN's hand-written `heap_caps_malloc(SPIRAM)` calls. |
| Any chip, must stay internal | `CONFIG_MANAGER_ALLOC_INTERNAL=y` | |

Options:

| Setting | Default | |
|---|---|---|
| `CONFIG_MANAGER_FILE_PATH` | `/spiffs/config_v2.json` | the v2 config file |
| `CONFIG_MANAGER_V1_FILE_PATH` | `/spiffs/config.json` | deleted after the first v2 save or patch; empty disables |
| `CONFIG_MANAGER_MAX_UNITS` | 15 | quota, also sizes `units[]` |
| `CONFIG_MANAGER_MAX_CONFIGURE_UNITS` | 20 | quota for `cu` |
| `CONFIG_MANAGER_MAX_IOS` | 100 | quota, all units together |
| `CONFIG_MANAGER_MAX_FILE_SIZE` | 4800 | quota in bytes of compact JSON; each save or patch allocates a buffer of this size |

In C, all of these get the usual `CONFIG_` prefix (e.g. `CONFIG_CONFIG_MANAGER_FILE_PATH`). Put them in the application's `sdkconfig.defaults`.

**RAM.** The peak of a load or update is the file text plus the cJSON tree (about 7 times the file size on a 32-bit chip), the new config (about 60 B per IO) and the output buffer (`CONFIG_MANAGER_MAX_FILE_SIZE`): about 47 kB at 4800 B and 100 IOs on the C6 (*est*). A full-file `set` (`"s":2`) has the same peak, because the stored file is not read; the MQTT receive buffer that holds the body comes on top. Confirm with a heap log on the device. What stays in RAM afterwards is `system_config_t` plus one `io_config_t` (60 B) per IO. 4800 B holds about 80 IOs in 15 units with typical names; 100 IOs fit only with short names and few optional keys.

**cJSON hooks on chips with PSRAM.** `CONFIG_MANAGER_ALLOC` only places the library's own buffers (config struct, IO pool, file text, output buffer, get/data payload). The cJSON trees, the largest part of a load or patch, are allocated through cJSON's allocator. On a target with PSRAM the application must call `cJSON_InitHooks()` with functions that prefer PSRAM (as NTN/Node's `init_cjson_psram_hooks()` does) before `config_manager_load()`; otherwise the trees land in internal RAM. `config_manager_get_payload()` returns a buffer that is always freed with `free()`, whatever hooks are installed.

## Moving MIOT_GW* from 1.x to 2.0

The firmware changes are listed in V2_PLAN.md section 10. Per call site:

| v1 | v2 |
|---|---|
| `version: v1.0.0` in `main/idf_component.yml` | `version: v2.0.1` |
| `config_manager_handle_partial_update(json)` (`cfg/update`, BLE, auto-addressing) | `config_manager_apply_patch(json)` on `v2/cfg/set`, then publish `config_manager_build_state(false)` on `v2/cfg/state`. It returns `esp_err_t`, not `bool`. |
| `config_manager_delete_unit(key)` | a patch `{"v":N,"u":{"3":null}}` |
| `config_manager_handle_sync(json)` returning `true` -> `cfg/hello` | same call on `v2/cfg/sync`; on `true` publish `config_manager_build_state(true)` |
| `config_manager_get_all_unit_hashes()`, `_compute_configure_units_hash()` in the hello | `config_manager_build_state(false)` on connect |
| `config_manager_get_units_payload(req)` on `cfg/request` | `config_manager_get_payload(req)` on `v2/cfg/get`, answer on `v2/cfg/data` |
| — | `config_manager_build_limits()` as `cfg` in `v2/caps`, once after every connect |
| `units[i].name` matched against a topic or rule | `config_manager_find_unit(key)`; `units[i].key` is the numeric key. `name` is removed so such code fails to compile. |
| `config_update_live_value(io, idx, v)` / `config_get_live_value(io, idx)` with an index from `find_unit` | `config_manager_set_live_value(key, io, v)` / `config_manager_get_live_value(key, io, &v)`; the index variants are only safe while holding `config_manager_lock()` across `find_unit` and the call |
| `config_manager_get_string(key)` | `config_manager_copy_string(key, buf, sizeof(buf))`; `get_string` only while holding `config_manager_lock()` |
| `units[i].hash` (string) | `uint32_t`; print with `"%08" PRIx32` |
| `system_hash`, `configure_units_hash` | `hash` (root hash, covers `cu`) |
| `config_get_gps_enabled()` NVS mirror (NTN) | `system_config_t.gps_enabled` |
| `config_manager_get_unit_json("unit_2")` | `config_manager_get_unit_json(2)` |

**Startup.** Call `config_manager_init()`, mount the file system, then `config_manager_load()`. If `config_manager_is_fallback()` is true, the stored file is unusable and the library runs on the default in RAM: **do not call `config_manager_reset_to_default()` then**, so the bad file stays until the server's full file (`"s":2`) replaces it. Otherwise call `config_manager_reset_to_default(id)` only when `config_manager_get_version()` is 0 and the stored ID (`config_manager_copy_string("device_ID", …)`) is not the device's ID.

**Device ID.** `config_manager_load()` has no device-ID parameter. When it writes the factory default (no file), the ID is `""`; when it falls back to the default for an unreadable file, the ID is the `id` of the bad file if it is still readable JSON (`id`, or `device_ID` of a v1 file), otherwise the device ID of the config that was in RAM, otherwise the last one the library had since `init`, otherwise `""`. To put the real ID in, call `config_manager_reset_to_default(id)` after `config_manager_load()` when `config_manager_get_version()` is 0, `config_manager_is_fallback()` is false and the stored ID differs (see Startup). Leaving the ID as it is also works: the server's full file carries `id`, and a wrong ID only changes the root hash, which already differs at `v` 0.

Auto-addressing that adds a unit itself must pick the lowest key never used on the gateway, not `count + 1` (V2_PLAN.md section 10), and send it as a patch with `v` = stored + 1. To make reading the version and applying the patch atomic, hold the recursive config lock around both:

```c
config_manager_lock();
int v = config_manager_get_version() + 1;
// build {"v":<v>,"u":{"<key>":{…}}}
esp_err_t err = config_manager_apply_patch(body);
config_manager_unlock();
```

While the lock is held, other tasks' live-value calls time out after 100 ms and readers wait, so keep the section short.

## Removing the component

Delete the dependency and the calls. Any code reading `system_config_t` (Modbus, rule adapter, BLE) goes with it.
