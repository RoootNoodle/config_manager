#!/usr/bin/env bash
# Build and run the host tests in one command: bash test/host/run.sh
# Runs the Unity tests, then test/schema_check.py (schema validation + parser drift guard).
# Exit code is non-zero if either fails.

cd "$(dirname "$0")" || exit 1

IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.0.1.sh}"

# Captured before the ESP-IDF env is loaded: jsonschema is in the system python, not the IDF one
SYS_PY="$(command -v python3)"

missing=0
if [ ! -f /usr/include/bsd/sys/cdefs.h ]; then
    echo "Missing libbsd headers. Install: sudo dnf install libbsd-devel   (Ubuntu: sudo apt install libbsd-dev)"
    missing=1
fi
if ! "$SYS_PY" -I -c "import jsonschema" 2>/dev/null; then
    echo "Missing python3 jsonschema (needed by test/schema_check.py). Install: sudo dnf install python3-jsonschema   (Ubuntu: sudo apt install python3-jsonschema)"
    missing=1
fi
if [ ! -f "$IDF_ACTIVATE" ]; then
    echo "ESP-IDF activation script not found: $IDF_ACTIVATE (set IDF_ACTIVATE to override)"
    missing=1
fi
[ "$missing" -eq 0 ] || exit 1

# The activation script exits when sourced from a script, so read its "-e" output (KEY=VALUE lines) instead
idf_env="$(sh "$IDF_ACTIVATE" -e)" || { echo "Could not read the ESP-IDF environment from $IDF_ACTIVATE"; exit 1; }
while IFS='=' read -r key value; do
    case "$key" in
        PATH) PATH="$value:$PATH" ;;
        SYSTEM_PATH | "") ;;
        *) export "$key=$value" ;;
    esac
done <<< "$idf_env"
if [ -z "${IDF_PATH:-}" ] || [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
    echo "IDF_PATH or IDF_PYTHON_ENV_PATH missing from $IDF_ACTIVATE -e"
    exit 1
fi

# The activation script puts ESP32 toolchain folders with a plain 'as' on PATH; the host gcc must use /usr/bin/as
PATH="$(printf '%s' "$PATH" | tr ':' '\n' \
    | grep -vE '(riscv32-esp-elf/riscv32-esp-elf|xtensa-esp-elf/xtensa-esp-elf|esp32ulp-elf/esp32ulp-elf)/bin$' \
    | paste -sd: -)"
export PATH

# idf.py is only an alias in interactive shells
IDF_PY=("${IDF_PYTHON_ENV_PATH}/bin/python" "${IDF_PATH}/tools/idf.py")

set -e
if [ ! -f sdkconfig ] || ! grep -q '^CONFIG_IDF_TARGET="linux"' sdkconfig; then
    "${IDF_PY[@]}" --preview set-target linux
fi
"${IDF_PY[@]}" build
set +e

rm -f /tmp/config_manager_host_test/defaults.json
./build/config_manager_host_test.elf
unity_rc=$?

"$SYS_PY" -I ../schema_check.py --defaults /tmp/config_manager_host_test/defaults.json
schema_rc=$?

[ "$unity_rc" -eq 0 ] && [ "$schema_rc" -eq 0 ]
