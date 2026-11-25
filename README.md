# config_manager

The gateway configuration for MIOT gateways, packaged as an ESP-IDF managed component. It parses the device's `config_v2.json` (Modbus units, IOs, sensor types) into a compact struct. It also:
- applies patches from the server (JSON Merge Patch) within Kconfig quotas,
- computes the 8-character hashes used for MQTT config sync,
- builds the `v2/cfg/state`, `v2/cfg/data` and `v2/caps` message bodies,
- holds the live IO values.

**The component carries the exact schema it implements.** `schemas/v2/config.schema.json` is the layout this version parses, and `CONFIG_MANAGER_SCHEMA_VERSION` in the header says which one it is. The component's major version equals the schema version. See [docs/SCHEMA.md](docs/SCHEMA.md).

It has no dependencies on application components and no NVS code. It works on the ESP32-C6 and on the ESP32-C5, with or without PSRAM, with memory placement chosen in Kconfig.

## Quick start

`main/idf_component.yml`:
```yaml
dependencies:
  config_manager:
    git: git@github.com:RoootNoodle/config_manager.git
    version: v2.0.1
```

```c
#include "config_manager.h"

ESP_ERROR_CHECK(config_manager_init());
// after the file system holding CONFIG_CONFIG_MANAGER_FILE_PATH is mounted:
config_manager_load();          // a missing file is replaced by factory defaults

const system_config_t *cfg = config_manager_get_ptr();
if (cfg) {
    // cfg->units[i], cfg->io_pool[cfg->units[i].io_pool_offset + j], ...
    // index of unit key 2: config_manager_find_unit(2)
    config_manager_release();
}
```

## Docs

| File | Contents |
|---|---|
| [docs/SCHEMA.md](docs/SCHEMA.md) | Schema versions, the v2 keys, defaults, quotas and hashes, v1 files on a v2 gateway |
| [docs/API.md](docs/API.md) | Functions, locking, the patch, state, sync, get and caps bodies |
| [docs/INTEGRATION.md](docs/INTEGRATION.md) | Kconfig per chip, moving MIOT_GW* from 1.x to 2.0 |
| [docs/V2_PLAN.md](docs/V2_PLAN.md) | The v2 design: file format, topics, hashes, quotas, migration |
| [CHANGELOG.md](CHANGELOG.md) | Versions |

## Layout

| Path | |
|---|---|
| `schemas/v2/` | `config.schema.json` (draft-07) and `config_example.json` |
| `schemas/v1/` | The v1 layout, kept for reference |
| `include/config_manager.h` | Public API, `system_config_t`, `CONFIG_MANAGER_SCHEMA_VERSION` |
| `src/config_manager.c` | Validate, parse, hash, patch, state/sync/get, live values, file I/O |
| `src/cm_alloc.c` | PSRAM / internal allocation (Kconfig) |
| `test/host/` | Linux-target Unity tests followed by the schema check: `bash test/host/run.sh` |
| `test/schema_check.py` | Validates the example and defaults against the schema; fails if the schema and parser drift apart |
| `test/hash_reference.py` | Python reference for the hashes: `python3 test/hash_reference.py schemas/v2/config_example.json` |
| `test/build/` | Device compile check (C6, C5+PSRAM, C5 internal-only): `bash test/build/build.sh` |
