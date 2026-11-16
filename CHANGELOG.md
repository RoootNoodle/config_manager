# Changelog

## 2.0.0 (unreleased): schema v2

The full design is in [docs/V2_PLAN.md](docs/V2_PLAN.md). Not compatible with v1 files, topics or call sites.

### Schema
- `schemas/v2/config.schema.json` and `config_example.json` are new. `schemas/v1/` is not changed.
- Short keys (`s`, `v`, `id`, `n`, `gps`, `cu`, `u`; unit `n`, `ty`, `a`, `w`, `c`, `io`; IO `r`, `t`, `f`, `b`, `l`, `e`), defaults left out.
- Units are keyed `"1"`-`"255"`. The IO key is its name (at most 31 bytes).
- `f` is required on every IO: one read code, one write code, or one of each.
- Unknown keys are rejected, in a file and in a patch. v1 files are rejected.
- `CONFIG_MANAGER_SCHEMA_VERSION` is 2. `test/schema_check.py` checks v2 and has no pass-through fields left.

### API
- `config_manager_apply_patch()` (JSON Merge Patch, RFC 7396) replaces `config_manager_handle_partial_update()` and `config_manager_delete_unit()`.
- `config_manager_build_state()` replaces `config_manager_get_all_unit_hashes()` and `config_manager_compute_configure_units_hash()`.
- `config_manager_get_payload()` replaces `config_manager_get_units_payload()`.
- New: `config_manager_find_unit()`, `config_manager_build_limits()`.
- `config_manager_handle_sync()` compares the root hash only.
- `config_manager_get_unit_json()` takes the numeric unit key.
- `config_manager_save()` takes a complete v2 file, validates it and stores it as compact JSON.
- `unit_config_t`: `name[40]` is now `uint8_t key`, `hash[65]` is now `uint32_t hash`. `system_config_t`: `system_hash[65]` is now `uint32_t hash`, `configure_units_hash` is removed, `gps_enabled` is new.
- `units[]` is sorted by key. The IOs of a unit are sorted by name.

### Hashes
- 8 hex characters: the first 4 bytes of SHA-256 over canonical JSON with every default written out. `v` is not part of the root hash. `test/hash_reference.py` is the reference.

### Behaviour
- Live values of units that a patch did not change are kept.
- Quotas from Kconfig: units, IOs, `cu` entries and file size. A save or patch over them is rejected (`"err":"q"` in the next state).
- A patch whose `v` equals the stored version is a duplicate: nothing is applied, `ESP_OK`, no `err`. A lower or missing `v` gives `"err":"v"`. Other codes: `inv`, `mem`, `fs`.
- After a patch the state lists the changed units in `u`, and leaves `u` out when no unit changed.
- An unreadable config file (not JSON, empty, invalid, over the quota) no longer leaves the library without a config: `config_manager_load()` logs a warning, runs on the default (`v` 0, no units) in RAM and returns `ESP_OK`, keeping the bad file. `config_manager_apply_patch()` then merges onto that default, so the server's full config repairs the file.
- Out of memory while reading the file no longer looks like a missing file (which wrote the factory default over a good config).
- The v2 file has its own path (`/spiffs/config_v2.json`). The factory default is `s`, `v:0`, device ID, default name, no units. The v1 file is deleted after the first successful v2 save or patch.

### Kconfig
- `CONFIG_MANAGER_FILE_PATH` default is `/spiffs/config_v2.json`.
- New: `CONFIG_MANAGER_V1_FILE_PATH` (`/spiffs/config.json`), `CONFIG_MANAGER_MAX_IOS` (120), `CONFIG_MANAGER_MAX_FILE_SIZE` (5800).

## 1.0.0 (unreleased): schema v1

The first standalone version. It is extracted from `MIOT_GW/components/config_manager`.

### Schema
- `schemas/v1/config.schema.json` is the gateway `Schema.json`, unchanged. `schemas/v1/config_example.json` is the example config.
- `CONFIG_MANAGER_SCHEMA_VERSION` is 1. From now on the component major version equals the schema version.
- `test/schema_check.py` validates the example and the factory defaults against the schema. It fails when a schema field is neither parsed nor listed as pass-through.

### Changed from the in-app copy
- **`IO_function_code` is parsed as the schema defines it.** Legacy `IO_type` is used only when `IO_function_code` is absent. When both are missing, the IO gets FC3 read + FC6 write; it used to be FC3 read-only.
- **`config_manager_save()`** parses the JSON before writing, so invalid JSON no longer replaces the file.
- **`config_manager_init()`** is idempotent. `config_manager_deinit()` is added.
- **`config_manager_get_all_unit_hashes()`** takes the config lock.
- **Sorted cJSON objects** keep a valid `child->prev` tail.
- **Kconfig settings:** file path, PSRAM/internal placement, maximum units, maximum configure_units.
- **NVS removed:** the NVS key names, connection flags and the gps_enabled mirror are no longer in the component. They belong to the app.

### Added
- Host tests (linux target, 21 tests, golden hashes from Python) and device compile checks (C6, C5+PSRAM, C5 internal-only)
