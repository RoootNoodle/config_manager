#!/usr/bin/env python3
"""Keeps schemas/v<N> and the C parser in step. Run by test/host/run.sh after the Unity tests.

1. The schema is a valid draft-07 schema.
2. The example config and the factory defaults written by the host tests validate against it.
3. Every property the schema defines is either parsed by src/config_manager.c or listed as
   pass-through (kept in the JSON file, not parsed into system_config_t). A new schema field
   therefore fails here until the parser handles it or it is listed below.
"""
import argparse
import json
import pathlib
import sys

import jsonschema

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 1

# Per schema level: fields read by the C parser / fields only stored in the JSON file
FIELDS = {
    "root": {
        "parsed": {"config_version", "device_ID", "name", "units", "configure_units"},
        "passthrough": {"MQTT_URL", "OTA_URL", "gps_enabled"},
    },
    "unit": {
        "parsed": {"unit_address", "unit_wifi_interval", "unit_NB-IOT_interval", "IO_config"},
        "passthrough": {"unit_name", "unit_default_address", "unit_manufacturer", "unit_type"},
    },
    "io": {
        "parsed": {"IO_function_code", "IO_read_address", "IO_start_byte", "IO_length",
                   "IO_value_type", "IO_name", "IO_endian"},
        "passthrough": {"IO_unit", "min", "max", "accuracy"},
    },
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--defaults", help="factory default config written by the host tests")
    args = ap.parse_args()

    schema_dir = ROOT / "schemas" / f"v{SCHEMA_VERSION}"
    schema = json.loads((schema_dir / "config.schema.json").read_text())
    errors = []

    jsonschema.Draft7Validator.check_schema(schema)
    validator = jsonschema.Draft7Validator(schema)

    docs = {"config_example.json": schema_dir / "config_example.json"}
    if args.defaults:
        docs["factory defaults"] = pathlib.Path(args.defaults)
    for label, path in docs.items():
        if not path.exists():
            errors.append(f"{label}: {path} not found")
            continue
        for e in validator.iter_errors(json.loads(path.read_text())):
            errors.append(f"{label}: {'/'.join(map(str, e.path)) or '<root>'}: {e.message}")

    levels = {
        "root": schema["properties"],
        "unit": schema["definitions"]["unit"]["properties"],
        "io": schema["definitions"]["io"]["properties"],
    }
    source = (ROOT / "src" / "config_manager.c").read_text()
    for level, props in levels.items():
        known = FIELDS[level]["parsed"] | FIELDS[level]["passthrough"]
        for name in sorted(set(props) - known):
            errors.append(f"schema {level} field '{name}' is neither parsed nor listed as pass-through")
        for name in sorted(known - set(props)):
            errors.append(f"{level} field '{name}' is listed here but not in the schema")
        for name in sorted(FIELDS[level]["parsed"]):
            if f'"{name}"' not in source:
                errors.append(f"{level} field '{name}' is listed as parsed but src/config_manager.c never reads it")

    for e in errors:
        print("SCHEMA_CHECK FAIL:", e)
    print(f"SCHEMA_CHECK: schema v{SCHEMA_VERSION}, {len(docs)} documents, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
