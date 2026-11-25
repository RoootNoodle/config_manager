#!/usr/bin/env python3
"""Reference implementation of the v2 config hashes (docs/V2_PLAN.md section 5).

    python3 test/hash_reference.py schemas/v2/config_example.json

Prints the root hash and every unit hash. The golden values in test/host/main/test_hash.c come from here.
"""
import hashlib
import json
import sys

DEFAULT_LENGTH = {"u16": 2, "i16": 2, "bool": 2, "u32": 4, "i32": 4, "f32": 4, "f64": 8}


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def h8(obj):
    return hashlib.sha256(canonical(obj).encode()).hexdigest()[:8]


def filled_unit(unit):
    out = {"n": unit["n"], "a": unit["a"], "w": unit.get("w", 60), "c": unit.get("c", 300), "io": {}}
    if "ty" in unit:
        out["ty"] = unit["ty"]
    for name, io in unit["io"].items():
        out["io"][name] = {
            "r": io["r"],
            "t": io["t"],
            "f": sorted(io["f"]),
            "b": io.get("b", 0),
            "l": io.get("l", DEFAULT_LENGTH[io["t"]]),
            "e": io.get("e", "b"),
        }
    return out


def unit_hash(unit):
    return h8(filled_unit(unit))


def root_hash(cfg):
    root = {
        "cu": cfg.get("cu", {}),
        "gps": cfg.get("gps", 0),
        "id": cfg["id"],
        "n": cfg["n"],
        "s": 2,
        "u": {k: unit_hash(u) for k, u in cfg["u"].items()},
    }
    return h8(root)


def main():
    cfg = json.loads(open(sys.argv[1], encoding="utf-8").read())
    print(f"root {root_hash(cfg)}")
    for key in sorted(cfg["u"], key=int):
        print(f"unit {key} {unit_hash(cfg['u'][key])}")
        print(f"     {canonical(filled_unit(cfg['u'][key]))}")


if __name__ == "__main__":
    main()
