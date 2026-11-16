# config_manager

The gateway configuration for MIOT gateways, packaged as an ESP-IDF managed component. It parses the device's `config.json` (Modbus units, IOs, configure_units) into a compact struct. It also:
- computes the canonical SHA-256 hashes used for MQTT config sync,
- applies partial updates from the server,
- holds the live IO values.

**The component carries the exact schema it implements.** `schemas/v1/config.schema.json` is the layout this version parses, and `CONFIG_MANAGER_SCHEMA_VERSION` in the header says which one it is. The component's major version equals the schema version. See [docs/SCHEMA.md](docs/SCHEMA.md).

It has no dependencies on application components and no NVS code. It works on the ESP32-C6 and on the ESP32-C5, with or without PSRAM, with memory placement chosen in Kconfig.

## Quick start

`main/idf_component.yml`:
```yaml
dependencies:
  config_manager:
    git: git@github.com:RoootNoodle/config_manager.git
    version: v1.0.0
```

```c
#include "config_manager.h"

ESP_ERROR_CHECK(config_manager_init());
// after the file system holding CONFIG_CONFIG_MANAGER_FILE_PATH is mounted:
config_manager_load();          // a missing file is replaced by factory defaults

const system_config_t *cfg = config_manager_get_ptr();
if (cfg) {
    // cfg->units[i], cfg->io_pool[cfg->units[i].io_pool_offset + j], ...
    config_manager_release();
}
```

## Docs

| File | Contents |
|---|---|
| [docs/SCHEMA.md](docs/SCHEMA.md) | Schema versions, what v1 means for the parser, how to release a new layout |
| [docs/API.md](docs/API.md) | Functions, locking, sync and partial update messages |
| [docs/INTEGRATION.md](docs/INTEGRATION.md) | Kconfig per chip, migrating MIOT_GW* from `components/config_manager` |
| [CHANGELOG.md](CHANGELOG.md) | Versions |

## Layout

| Path | |
|---|---|
| `schemas/v1/` | `config.schema.json` (draft-07) and `config_example.json` |
| `include/config_manager.h` | Public API, `system_config_t`, `CONFIG_MANAGER_SCHEMA_VERSION` |
| `src/config_manager.c` | Parse, hash, sync, partial update, live values, file I/O |
| `src/cm_alloc.c` | PSRAM / internal allocation (Kconfig) |
| `test/host/` | Linux-target Unity tests followed by the schema check: `bash test/host/run.sh` |
| `test/schema_check.py` | Validates the example and defaults against the schema; fails if the schema and parser drift apart |
| `test/build/` | Device compile check (C6, C5+PSRAM, C5 internal-only): `bash test/build/build.sh` |
