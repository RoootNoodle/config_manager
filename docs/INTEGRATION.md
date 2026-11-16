# Integration

## Kconfig per chip

Everything is in `menuconfig` under **Config Manager Configuration**.

| Setup | Settings | Result |
|---|---|---|
| ESP32-C6, no PSRAM (MIOT_GW) | defaults | Plain `malloc` from internal RAM |
| ESP32-C5 + PSRAM (MIOT_GW_NTN) | defaults | Config and JSON buffers prefer PSRAM and fall back to internal RAM. This replaces NTN's hand-written `heap_caps_malloc(SPIRAM)` calls. |
| Any chip, must stay internal | `CONFIG_MANAGER_ALLOC_INTERNAL=y` | |

Other options:
- `CONFIG_MANAGER_FILE_PATH`, default `/spiffs/config.json`
- `CONFIG_MANAGER_MAX_UNITS`, default 15
- `CONFIG_MANAGER_MAX_CONFIGURE_UNITS`, default 20

In C, all of these get the usual `CONFIG_` prefix (e.g. `CONFIG_CONFIG_MANAGER_FILE_PATH`).

cJSON trees use the app's cJSON hooks if any are installed, as with NTN/Node's `init_cjson_psram_hooks()`.

## Migrating MIOT_GW / MIOT_GW_NTN / MIOT_GW_Node

1. **Delete `components/config_manager/`.** A local component with the same name takes precedence over the managed one.
2. **Add the dependency** to `main/idf_component.yml`:
   ```yaml
   config_manager:
     git: git@github.com:RoootNoodle/config_manager.git
     version: v1.0.0
   ```
   The existing `REQUIRES config_manager` lines keep working.
3. **Move the NVS definitions into the app.** The old header also defined device settings, which are not part of the config schema:
   - `NVS_NAMESPACE` and the `NVS_KEY_*` names
   - NTN only: `CONN_FLAG_*`, `conn_mode_t`, `config_get/set_conn_flag()` and `config_get_gps_enabled()` with its NVS mirror

   Put them in an app header (e.g. `main/nvs_keys.h`, or telemetry). On MIOT_GW they are used by `main.c`, `registration.c`, `net_lifecycle.c`, `device_state.c`, telemetry, time_manager, ble_manager and system_command_manager. On NTN, call the gps_enabled mirror from the app after `config_manager_load()` and after every successful update.
4. **Check behaviour changes** (below) against your configs.

No other call sites change.

## Behaviour changes from the in-app copy

| Change | Effect |
|---|---|
| **`IO_function_code` is now parsed** (schema v1) | IOs that use it (like `config_example.json`) were read as FC3 read-only, because the old firmware ignored this field. Now `[4]` reads input registers and `[3, 6]` becomes writable. |
| An IO with neither `IO_function_code` nor `IO_type` | Was FC3 read-only. Is now FC3 + FC6, as the schema says. **Such IOs become writable**, for example by `write_io` rules. |
| `config_manager_save()` with invalid JSON | Used to overwrite the file. Now rejected, and the file is kept. |
| `config_manager_init()` called twice | Used to leak and replace the mutexes. Now does nothing the second time. |
| `config_manager_get_all_unit_hashes()` | Now takes the config lock. |

Hashes are unchanged, so config sync with the server keeps working.

## Removing the component

Delete the dependency and the calls. Any code reading `system_config_t` (Modbus, rule adapter, BLE) goes with it.
