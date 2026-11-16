# Changelog

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
