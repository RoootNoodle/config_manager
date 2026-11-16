# config_manager v2: plan and guide

Status: **plan, nothing implemented.** This is the full description of what changes from v1.0.0 to v2.0.0, how the firmware has to behave, and what Web has to send. The matching rules plan is `rule_manager/docs/V2_PLAN.md`.

Items marked **(decision)** are proposals that still need a yes.

## 1. Why v2

| Problem in v1 | v2 |
|---|---|
| The file carries fields the gateway never reads. | Only what is needed to read a register, plus the names. |
| Long keys. The example config is 1839 B. | Short keys, defaults left out: 523 B (measured). |
| Units are found by position after sorting the keys, so rules mix units up. | Units are found by their key, a number `"1"`–`"255"`. |
| An update replaces a whole unit. | Any single value can be patched. |
| 64-character SHA-256 per unit in every hello and sync. | 8-character hashes; a normal sync carries one. |
| The hash covers the stored text, including fields the gateway ignores. | The hash covers the values the gateway uses. |
| The file does not say which schema it follows. | `"s":2`. |

## 2. The config file

```json
{"s":2,"v":8,"id":"8574…","n":"MIOT-Gateway","gps":0,
 "cu":{"MIOT_Level_Sensor":298},
 "u":{
  "1":{"n":"Level Sensor","ty":"MIOT_Level_Sensor","a":10,"w":5,"c":300,
       "io":{"level":{"r":100,"t":"f32","f":[3]}}},
  "2":{"n":"Power Meter","a":20,"w":10,"c":600,
       "io":{"voltage":{"r":200,"t":"f32","f":[4]},
             "relay":{"r":210,"t":"u16","f":[3,6]}}}}}
```

### Root

| Key | Meaning | v1 | When omitted |
|---|---|---|---|
| `s` | schema version, always 2 | new | rejected |
| `v` | config version counter | `config_version` | rejected |
| `id` | device ID | `device_ID` | rejected |
| `n` | gateway name, at most 39 characters | `name` | rejected |
| `gps` | GNSS on (1) or off (0) | `gps_enabled` | 0 |
| `cu` | sensor type → register that holds the unit address | `configure_units` | empty |
| `u` | units, keyed `"1"`–`"255"` | `units` | rejected |

### Unit

| Key | Meaning | v1 | When omitted |
|---|---|---|---|
| `n` | unit name | `unit_name` | rejected |
| `ty` | sensor type, a key of `cu` | `unit_type` | none |
| `a` | Modbus address, 2–247 | `unit_address` | rejected |
| `w` | publish interval on WiFi, seconds | `unit_wifi_interval` | 60 |
| `c` | publish interval on cellular (NB-IoT), seconds | `unit_NB-IOT_interval` | 300 |
| `io` | IOs; the key is the IO name, at most 31 characters | `IO_config` and `IO_name` | rejected |

The unit key never changes and is never reused on a gateway. Rules, the control topic and hashes all refer to it.

### IO

| Key | Meaning | v1 | When omitted |
|---|---|---|---|
| `r` | register address, 0–65535 | `IO_read_address` | rejected |
| `t` | `u16`, `i16`, `u32`, `i32`, `f32`, `bool` | `IO_value_type` | rejected |
| `f` | Modbus function codes: one read code (1–4), one write code (5, 6, 15, 16), or one of each | `IO_function_code` | rejected |
| `b` | start byte | `IO_start_byte` | 0 |
| `l` | length in bytes, 1–8 | `IO_length` | 2 for 16-bit types and `bool`, 4 for 32-bit types |
| `e` | `"l"` for little-endian | `IO_endian` | big-endian |

`f` examples: `[3]` read holding register; `[4]` read input register; `[3,6]` read and write a holding register; `[1,5]` read and write a coil; `[6]` write only.

`f` is required on every IO, so the file always says how a register is accessed. An IO without a write code cannot be written by a rule or a control message.

### Removed

`MQTT_URL`, `OTA_URL`, `unit_default_address`, `unit_manufacturer`, `IO_unit`, `min`, `max`, `accuracy`, the legacy `IO_type`, and `IO_name` (the key is the name). Web keeps display data such as units of measurement, limits and accuracy in its own database.

Unknown keys are rejected, in a file and in a patch.

## 3. Topics

All config traffic is on `devices/{id}/v2/cfg/*`. The v1 topics (`cfg/hello`, `cfg/request`, `cfg/update`, `cfg/sync`, `cfg/units`) are left to gateways on v1 firmware.

| Topic | Direction | Purpose | v1 |
|---|---|---|---|
| `v2/cfg/set` | to gateway, QoS 1 | patch | `cfg/update` |
| `v2/cfg/get` | to gateway, QoS 1 | ask for parts | `cfg/request` |
| `v2/cfg/sync` | to gateway, QoS 1 | server's hash | `cfg/sync` |
| `v2/cfg/state` | from gateway | version and hashes | `cfg/hello` |
| `v2/cfg/data` | from gateway | parts of the config | `cfg/units` |
| `v2/caps` | from gateway | limits of this gateway (section 7) | new |

Not changed: `devices/{id}/sense/{addr}/{io}` and its payload. `devices/{id}/control/{unit}/{io}` keeps its name; `{unit}` is the unit key (`control/2/relay`).

The library builds and parses the message bodies. The application owns the MQTT client and the topic strings.

## 4. Updating the config

### `v2/cfg/set`: patch

The body is a piece of the file in the same shape (JSON Merge Patch, RFC 7396):

- An object merges into the stored object, key by key.
- Any other value replaces the stored value. Arrays (`f`) are replaced whole.
- `null` deletes the key.
- `v` is required and must be higher than the stored version. Otherwise nothing changes and the gateway answers with its current state.

| Change | Body | Bytes |
|---|---|---|
| Only the NB-IoT interval of unit 2 | `{"v":9,"u":{"2":{"c":900}}}` | 26 |
| Rename the gateway | `{"v":10,"n":"Pump house"}` | 25 |
| Add an IO to unit 2 | `{"v":11,"u":{"2":{"io":{"power":{"r":204,"t":"f32","f":[4]}}}}}` | 61 |
| Make an IO writable | `{"v":12,"u":{"2":{"io":{"relay":{"f":[3,6]}}}}}` | 45 |
| Delete an IO | `{"v":13,"u":{"2":{"io":{"relay":null}}}}` | 39 |
| Delete unit 3 | `{"v":14,"u":{"3":null}}` | 22 |
| Add a unit | `{"v":15,"u":{"4":{ …full unit… }}}` | unit size |
| Full config | every key; `null` for units to drop | file size |

What the gateway does:

1. Parse the body. Reject it when `v` is not higher, or a key is unknown.
2. Load the stored file and merge.
3. Validate the result as a complete v2 file. If it is invalid, keep the old file and report `err`.
4. Write the file, rebuild the in-memory config, recompute the hashes of the units that changed and the root hash.
5. Publish `state`.

Live values of units that did not change are kept across a patch. Units that changed start with no value until the next Modbus read.

### `v2/cfg/state`

```json
{"s":2,"v":9,"h":"a1b2c3d4","u":{"2":"9f8e7d6c"}}
```

| Key | Meaning |
|---|---|
| `v` | version now stored |
| `h` | root hash |
| `u` | unit hashes. After a patch: the units that changed (`null` for a deleted one). After a sync mismatch: all units. On connect: left out. |
| `err` | present when a patch was rejected; `v` and `h` then describe the unchanged config |

### `v2/cfg/sync`

1. The server sends `{"v":9,"h":"a1b2c3d4"}`.
2. Same hash: the gateway stays silent.
3. Different hash: the gateway publishes `state` with all unit hashes.
4. The server sends a `set` for the units that differ, or a `get` to read the gateway's copy.

### `v2/cfg/get` and `v2/cfg/data`

| Body of `get` | Answer on `data` |
|---|---|
| `{}` | the whole config |
| `{"u":["2","3"]}` | `{"s":2,"v":9,"u":{"2":{ … },"3":{ … }}}` |
| `{"cu":1}` | `{"s":2,"v":9,"cu":{ … }}` |

`data` is also what the gateway publishes when it adds a unit itself (auto-addressing), followed by `state`.

## 5. Hashes

The hash only detects a difference. It is the first 8 hex characters (lowercase) of SHA-256 over this string:

1. Take the object with every optional key filled in with its default: `w`, `c`, `b`, `l`, `e` (`"b"` for big), `gps`. `ty` is left out when absent.
2. Sort the keys at every level by byte order. Sort `f` ascending.
3. Print as compact JSON, UTF-8 kept as-is. All numbers are whole numbers.

| Hash | Taken over |
|---|---|
| unit | the unit object |
| root | `{"cu":{ … },"gps":0,"id":"…","n":"…","s":2,"u":{ "<key>":"<unit hash>", … }}`; `v` is not part of it |

So `{"r":100,"t":"f32","f":[3]}` and `{"r":100,"t":"f32","f":[3],"b":0,"l":4,"e":"b"}` hash the same.

Python reference for one unit, after filling in the defaults:

```python
import hashlib, json
s = json.dumps(unit, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
h = hashlib.sha256(s.encode()).hexdigest()[:8]
```

**No hash per IO.** A v2 IO is about 28 B and its line in a hash list about 20 B, so listing IO hashes costs nearly as much as resending the IO. The patch gives single-value control; the hash only has to say which unit differs.

RAM: a unit hash goes from 65 B to 4 B, about 0.9 kB less with 15 units.

## 6. Moving from v1

The library does not convert v1 files. The v2 file has its own path, so the v1 file is left alone until v2 works.

| | v1 | v2 (Kconfig default) |
|---|---|---|
| File | `/spiffs/config.json` | `/spiffs/config_v2.json` |

1. v2 firmware starts with no v2 file. `config_manager_load()` writes the factory default: `s`, `"v":0`, the device ID, the default name, no units.
2. On connect the gateway publishes `v2/cfg/state`.
3. The server sees the hash differs from its copy and sends the full config on `v2/cfg/set`.
4. After the file is saved and reloaded without error, the library deletes the v1 file (`CONFIG_CONFIG_MANAGER_V1_FILE_PATH`; empty disables this).

**Between the firmware update and step 3 the gateway has no units and reads no sensors.** Web must speak v2 before v2 firmware is rolled out. If the new firmware is rolled back before step 4, the v1 file is still there.

### What Web converts

| v1 | v2 |
|---|---|
| Unit key `unit_N` | `"N"`. Any other key: the lowest free number. |
| IO | The v2 key is the v1 `IO_name`, or the v1 key when `IO_name` is missing. This keeps the name in the `sense` topic. |
| An IO named `Set_Address` | Left out; v1 firmware skips it. |
| `IO_function_code` | Copied to `f`. `f` is required in v2: when it is missing in v1, write the codes that describe what the device does today (the v1 firmware in the field reads such IOs with FC3 only). |
| `IO_value_type` | `FP32` and `FLOAT32` → `f32`, `UINT16` → `u16`, `INT16` → `i16`, `UINT32` → `u32`, `INT32` → `i32`, `BOOL` → `bool` |
| `IO_endian` `"little"` | `"e":"l"` |
| `gps_enabled`, `unit_type` | `gps`, `ty` |
| `config_version` | Continue counting in `v`; any value above 0 is accepted by a fresh v2 gateway. |

## 7. Guide for Web

1. Keep the v2 config per gateway. Display data (units of measurement, limits, accuracy, manufacturer) stays in the Web database and is not sent.
2. For every change, send the smallest `set` that describes it and increase `v` by one.
3. Compute the unit and root hash as in section 5 and compare with the `state` that comes back. A different `h` means the patch did not produce what Web expected: send `sync`, read the unit hashes, resend the units that differ.
4. On `err`, nothing was changed on the gateway. Show the text to the user.
5. Never reuse a unit key. A deleted unit's key stays retired, because rules and history refer to it.
6. Send `sync` on a schedule and after a gateway comes online. It costs about 30 B when nothing differs.
7. A gateway publishes on `v2/cfg/state` only when it runs v2 firmware. Use that to decide which format to send.

### Quota: what Web may send to this gateway

Limits differ per gateway type and firmware version, so Web does not keep its own table. Every v2 gateway reports its limits, and Web refuses to save anything that goes over them. A gateway that has not reported limits is on v1 firmware and gets no v2 messages at all.

The gateway publishes on `devices/{id}/v2/caps`, QoS 1, not retained, once after every connect:

```json
{"fw":"1.2.0","hw":"GW-C6","s":2,"msg":16384,
 "cfg":{"u":15,"io":120,"cu":20,"b":5800},
 "rl":{"r":25,"b":5800,"tp":8,"tw":16}}
```

| Key | Meaning |
|---|---|
| `fw`, `hw` | firmware version and hardware type |
| `s` | schema version this firmware speaks |
| `msg` | largest single message the gateway can receive, in bytes |
| `cfg.u` | most units |
| `cfg.io` | most IOs, all units together |
| `cfg.cu` | most sensor types |
| `cfg.b` | largest config file, in bytes of compact JSON |
| `rl` | the rule limits, see the rules guide |

Web keeps the last `caps` per gateway and uses it to:

1. Show what is used and what is left ("96 of 120 IOs").
2. Block a change that would go over a limit, before sending anything.
3. Split a config larger than `msg` into several `set` messages.

The gateway checks the same limits. A `set` whose result goes over them is rejected with `"err":"quota"` and the stored config stays as it was. So a wrong number in Web cannot overload a gateway.

The numbers come from Kconfig in each firmware project, so each board gets values that fit its RAM:

| Setting | Proposed, ESP32-C6 (MIOT_GW) | Proposed, ESP32-C5 + PSRAM (MIOT_GW_NTN) |
|---|---|---|
| `CONFIG_MANAGER_MAX_UNITS` | 15 | 15 |
| `CONFIG_MANAGER_MAX_IOS` (new) | 120 | 600 |
| `CONFIG_MANAGER_MAX_FILE_SIZE` (new) | 5800 | 28000 |

**Where the numbers come from.** The RAM that matters is the peak while a file is loaded or updated: the file text plus the JSON tree built from it, about 8.5 times the file size (*est*). Every update reaches this peak, also one that changes a single value.

| Board | RAM budget for one load or update | File size that fits | In IOs |
|---|---|---|---|
| ESP32-C6 | 50 kB (decided) | about 5,800 B | 120 IOs in 15 units peak at 48 kB (*est*) |
| ESP32-C5, 8 MB PSRAM shared with other features | 256 kB, 3% of the PSRAM (decided for now, may be adjusted) | about 28,000 B | 600 IOs in 15 units peak at 211 kB (*est*) |

What stays in RAM afterwards is far smaller: about 5.6 kB for 120 IOs and 27 kB for 600 (*plan*). The peaks are calculated, not measured; confirm them with a heap log on a device.

The gateway works fully offline on the stored config. Changes made in Web while a gateway is offline are delivered when it reconnects (QoS 1, persistent session), and `sync` repairs anything that was missed.

## 8. Firmware changes

### Public API

| Function or type | Change |
|---|---|
| `config_manager_init`, `_deinit`, `_load`, `_save`, `_reset_to_default`, `_get_ptr`, `_release`, `_unload`, `_lock`, `_unlock`, `_await_idle`, `_get_version`, `_get_string`, `_get_config_json` | same names; `_save` takes a full v2 file |
| `CONFIG_MANAGER_SCHEMA_VERSION` | 2 |
| `unit_config_t` | `name[40]` (held the JSON key) becomes `uint8_t key`; `hash[65]` becomes `uint32_t hash` |
| `system_config_t` | `system_hash[65]` becomes `uint32_t hash`; `configure_units_hash` is removed (part of the root hash); new `bool gps_enabled` |
| `int config_manager_find_unit(uint8_t unit_key)` | new; index into `units[]`, or -1 |
| `config_update_live_value()`, `config_get_live_value()` | unchanged; they take the index from `config_manager_find_unit()` |
| `esp_err_t config_manager_apply_patch(const char *json)` | replaces `config_manager_handle_partial_update()` and `config_manager_delete_unit()` |
| `bool config_manager_handle_sync(const char *json)` | same name; compares the root hash |
| `cJSON *config_manager_build_state(bool all_units)` | replaces `config_manager_get_all_unit_hashes()` and `config_manager_compute_configure_units_hash()` |
| `char *config_manager_get_payload(const char *request_json)` | replaces `config_manager_get_units_payload()` |
| `cJSON *config_manager_build_limits(void)` | new; the `cfg` object of the `caps` message |
| `cJSON *config_manager_get_unit_json(uint8_t unit_key)` | takes the numeric key |

`unit_config_t.name` is removed and not reused for the unit's display name: code that still matches it against a topic or a rule must fail to compile.

The unit name (`n`) and type (`ty`) are not kept in RAM; they are read from the file when needed.

### Inside

- Units are stored sorted by key; `find_unit` is a short loop over at most `CONFIG_CONFIG_MANAGER_MAX_UNITS` entries.
- A patch runs under the config lock, as `handle_partial_update` does today. No new task and no new stack.
- The patch keeps today's low-memory order: free the active config, load the file, merge, write, rebuild from the merged tree.

### Kconfig

| Setting | Default |
|---|---|
| `CONFIG_MANAGER_FILE_PATH` | `/spiffs/config_v2.json` |
| `CONFIG_MANAGER_V1_FILE_PATH` | `/spiffs/config.json` |
| `CONFIG_MANAGER_MAX_IOS` | 120 |
| `CONFIG_MANAGER_MAX_FILE_SIZE` | 5800 |

### Tests (host)

Parsing with every default; rejection of unknown keys, of v1 files, and of a missing or bad `f`; patch of one value, add and delete of IO and unit, rejected patch leaving the file intact, stale `v`, a patch over the unit, IO or size quota; hashes against the Python reference, including "default written out" equals "default left out"; state after a patch and after a sync mismatch; get; removal of the v1 file; `find_unit`.

### Files

| File | Change |
|---|---|
| `schemas/v2/config.schema.json`, `config_example.json` | new; `schemas/v1/` is not touched |
| `include/config_manager.h`, `src/config_manager.c` | as above |
| `test/schema_check.py` | `SCHEMA_VERSION = 2`, new field lists (no pass-through fields are left) |
| `Kconfig`, `idf_component.yml` (2.0.0), `CHANGELOG.md` | updated |
| `docs/SCHEMA.md`, `docs/API.md`, `docs/INTEGRATION.md`, `README.md` | updated |
| `test/host/main/*` | updated and extended |

## 9. Suggested order

1. Schema, example and the Python hash reference.
2. Parser with short keys and defaults, strict validation, unit key, hashes.
3. Patch, state, sync, get.
4. Removal of the v1 file, docs, changelog, tag `v2.0.0`.

## 10. What the gateway application changes later

Not part of this library, listed so nothing is forgotten when MIOT_GW adopts v2:

- Replace `components/config_manager` with this library; move the `NVS_KEY_*` names into the application.
- Subscribe to and publish on the `v2/cfg/*` topics.
- `control/{unit}/{io}`: look the unit up with `config_manager_find_unit()`.
- Auto-addressing: a new unit gets the lowest key never used, not `count + 1`.
- BLE config: accept the same patch body; new BLE spec version.
- Rule adapter: resolve the unit key with `config_manager_find_unit()`.

## 11. Open decisions

| # | Item | Proposal |
|---|---|---|
Decided: `f` is required on every IO; names as listed; file name `config_v2.json`.

Decided: the RAM budget for one load or update is 50 kB on the ESP32-C6 (120 IOs, 5.8 kB file) and 256 kB on the ESP32-C5 for now (600 IOs, 28 kB file); `caps` is sent once after every connect on `v2/caps`.

No open items.
