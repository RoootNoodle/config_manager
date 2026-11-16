# Schema versions

| Schema | Component versions | Files |
|---|---|---|
| v1 | 1.x.y | `schemas/v1/config.schema.json`, `schemas/v1/config_example.json` |

The rules:
- A component release parses exactly one schema version. That number is `CONFIG_MANAGER_SCHEMA_VERSION` in `config_manager.h`, and it equals the major version in `idf_component.yml`.
- `config_version` inside `config.json` is the *data* version. The server bumps it on every change, and it has nothing to do with the schema version.
- **Files in `schemas/vN/` are frozen once vN is released.** Fix a typo in a description as a patch release. Never change the meaning of a field within a version.

## v1: what the parser does with each field

`test/schema_check.py` enforces this table: every schema property must be either parsed or listed as pass-through.

| Level | Parsed into `system_config_t` | Kept in the file only (pass-through) |
|---|---|---|
| root | `config_version`, `device_ID`, `name`, `units`, `configure_units` | `MQTT_URL`, `OTA_URL`, `gps_enabled` |
| unit | `unit_address`, `unit_wifi_interval`, `unit_NB-IOT_interval`, `IO_config` (the JSON key becomes `unit_config_t.name`) | `unit_name`, `unit_default_address`, `unit_manufacturer`, `unit_type` |
| IO | `IO_function_code`, `IO_read_address`, `IO_start_byte`, `IO_length`, `IO_value_type`, `IO_name`, `IO_endian` | `IO_unit`, `min`, `max`, `accuracy` |

Pass-through fields are still part of the unit and system hashes. They are returned by `config_manager_get_config_json()`, `get_unit_json()` and `get_units_payload()`.

**`IO_function_code`** follows the schema:
- Read codes 1–4 set `read_fc`. Write codes 5, 6, 15 and 16 set `write_fc`.
- `[6]` is write-only, `[3, 6]` is read + write.
- **Omitted:** FC3 read + FC6 write.
- **Legacy `IO_type`** (`"R"`, `"W"`, or a number) is only used when `IO_function_code` is absent. Configs written for the old firmware therefore keep their meaning.
- An array with no valid code is treated as omitted, with a warning in the log.

**Accepted beyond the schema:**
- `IO_value_type` `"FLOAT32"` (read as FP32) and `"BOOL"`.
- An unknown or missing type is read as UINT16.
- An IO named `Set_Address` is skipped. It is the unit-addressing register, not a reading.

**Defaults when a field is missing:**

| Field | Default |
|---|---|
| `unit_wifi_interval` | 60 |
| `unit_NB-IOT_interval` | 300 |
| `IO_length` | 2 |
| `IO_endian` | big |

**Limits:**
- Units beyond `CONFIG_CONFIG_MANAGER_MAX_UNITS` (default 15) are ignored.
- configure_units entries beyond `..._MAX_CONFIGURE_UNITS` (default 20) are ignored.
- Names are truncated to 39 characters.

## Hashes

Each hash is the SHA-256 (lowercase hex) of `cJSON_PrintUnformatted`, with object keys sorted by `strcmp` at every level and UTF-8 kept as-is:

| Hash | Computed over |
|---|---|
| unit | one unit object |
| system | the whole config |
| configure_units | the configure_units object |

In Python, the equivalent is `json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)`. The host tests pin all of these against values computed that way.

## Releasing a new layout (v2)

1. Add `schemas/v2/config.schema.json` and `config_example.json`. Leave `schemas/v1/` untouched.
2. Update the parser, `CONFIG_MANAGER_SCHEMA_VERSION` (2), `SCHEMA_VERSION` and `FIELDS` in `test/schema_check.py`, and `EXAMPLE_JSON` / the tests.
3. Decide how a device on v2 firmware treats a v1 file still on flash (migrate on load, or reject). Write that down in this file.
4. Bump the major version in `idf_component.yml`, add a CHANGELOG entry, and tag `v2.0.0`.
