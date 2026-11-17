# Schema versions

| Schema | Component versions | Files |
|---|---|---|
| v1 | 1.x.y | `schemas/v1/config.schema.json`, `schemas/v1/config_example.json` |
| v2 | 2.x.y | `schemas/v2/config.schema.json`, `schemas/v2/config_example.json` |

The rules:
- A component release parses exactly one schema version. That number is `CONFIG_MANAGER_SCHEMA_VERSION` in `config_manager.h`, and it equals the major version in `idf_component.yml`.
- `v` inside the config file is the *data* version. The server bumps it on every change, and it has nothing to do with the schema version (`s`).
- **Files in `schemas/vN/` are frozen once vN is released.** Fix a typo in a description as a patch release. Never change the meaning of a field within a version.

The design, the MQTT messages and the reasons behind v2 are in [V2_PLAN.md](V2_PLAN.md).

## v2: the file

```json
{"s":2,"v":8,"id":"8574…","n":"MIOT-Gateway","gps":0,
 "cu":{"MIOT_Level_Sensor":298},
 "u":{"1":{"n":"Level Sensor","ty":"MIOT_Level_Sensor","a":10,"w":5,"c":300,
           "io":{"level":{"r":100,"t":"f32","f":[3]}}}}}
```

`test/schema_check.py` checks that every key in the schema is read by `src/config_manager.c`. v2 has no pass-through fields: a key is either parsed or rejected.

| Level | Key | Meaning | Default | Parsed into |
|---|---|---|---|---|
| root | `s` | schema version, always 2 | required | |
| root | `v` | config version, whole number >= 0 | required | `version` |
| root | `id` | device ID; no `/`, `+`, `#` or control characters | required | `device_id` (truncated to 39 bytes) |
| root | `n` | gateway name, at most 39 bytes | required | `name` |
| root | `gps` | 0 or 1 | 0 | `gps_enabled` |
| root | `cu` | sensor type -> register 0–65535 | empty | `configure_units[]` (sorted by name, type truncated to 39 bytes) |
| root | `u` | units, keyed `"1"`–`"255"` (no leading zero) | required | `units[]`, sorted by key |
| unit | `n` | unit name | required | not kept in RAM |
| unit | `ty` | sensor type | none | not kept in RAM |
| unit | `a` | Modbus address 2–247 | required | `address` |
| unit | `w` | WiFi interval, seconds, 1–65535 | 60 | `wifi_interval_s` |
| unit | `c` | NB-IoT interval, seconds, 1–65535 | 300 | `nbiot_interval_s` |
| unit | `io` | IOs; the key is the name, 1–31 bytes, no `/`, `+`, `#` or control characters (0x00–0x1F, 0x7F) | required (may be empty) | `io_pool[]`, sorted by name |
| IO | `r` | register 0–65535 | required | `read_addr` |
| IO | `t` | `u16` `i16` `u32` `i32` `f32` `bool` | required | `value_type` |
| IO | `f` | one read code (1–4), one write code (5, 6, 15, 16), or one of each | required | `read_fc`, `write_fc` |
| IO | `b` | start byte 0–255 | 0 | `start_byte` |
| IO | `l` | length 1–8 | 2 for 16-bit and `bool`, 4 for 32-bit | `length` |
| IO | `e` | `"l"` little-endian, `"b"` big-endian | big-endian | `big_endian` |

Also rejected: unknown keys at any level, duplicate keys in one object, numbers that are not whole, and a file over the quotas below.

**Quotas** (Kconfig; reported in `v2/caps` by `config_manager_build_limits()`):

| Setting | Default | Over the limit |
|---|---|---|
| `CONFIG_MANAGER_MAX_UNITS` | 15 | rejected, `ESP_ERR_INVALID_SIZE` / `"err":"q"` |
| `CONFIG_MANAGER_MAX_IOS` | 100 | same |
| `CONFIG_MANAGER_MAX_CONFIGURE_UNITS` | 20 | same |
| `CONFIG_MANAGER_MAX_FILE_SIZE` | 4800 B of compact JSON | same |

## Hashes

A hash is the first 8 lowercase hex characters (the first 4 bytes, `uint32_t` in RAM) of SHA-256 over:

1. The object with every optional key filled in with its default (`w`, `c`, `b`, `l`, `e` as `"b"`, `gps`). `ty` is left out when absent.
2. Keys sorted by byte order at every level; `f` sorted ascending.
3. Compact JSON, UTF-8 kept as-is.

| Hash | Taken over |
|---|---|
| unit | the unit object |
| root | `{"cu":{…},"gps":0,"id":"…","n":"…","s":2,"u":{"<key>":"<unit hash>",…}}`; `v` is not part of it |

The library streams this text into SHA-256 without building it in RAM. `test/hash_reference.py` is the Python reference, and the host tests pin its values.

## v1 files on a v2 gateway

The library does not convert v1 files. v2 uses its own path (`CONFIG_MANAGER_FILE_PATH`, default `/spiffs/config_v2.json`), so the v1 file stays untouched until a v2 config arrives:

1. With no v2 file, `config_manager_load()` writes the factory default `{"s":2,"v":0,"id":"","n":"MIOT-Gateway","u":{}}`. Call `config_manager_reset_to_default(id)` to put the device ID in.
2. The server sends the converted config (V2_PLAN.md section 6) on `v2/cfg/set`.
3. After a v2 config from `config_manager_save()` or `config_manager_apply_patch()` is written and loaded, the file at `CONFIG_MANAGER_V1_FILE_PATH` is deleted (empty disables this). Writing the factory default does not delete it.

A v1 file given to `config_manager_save()`, or found at the v2 path, is rejected.

## Releasing a new layout (v3)

1. Add `schemas/v3/config.schema.json` and `config_example.json`. Leave older folders untouched.
2. Update the parser, `CONFIG_MANAGER_SCHEMA_VERSION`, `SCHEMA_VERSION` and `FIELDS` in `test/schema_check.py`, `test/hash_reference.py` if the hash changes, and the tests.
3. Decide how a device treats the previous file still on flash. Write that down in this file.
4. Bump the major version in `idf_component.yml`, add a CHANGELOG entry, and tag the release.
