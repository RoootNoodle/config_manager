#!/usr/bin/env bash
# Compile the component for each supported chip/memory setup: bash test/build/build.sh
# Exit code is non-zero if any build fails.

cd "$(dirname "$0")" || exit 1

IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.0.1.sh}"

missing=0
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


# idf.py is only an alias in interactive shells
IDF_PY=("${IDF_PYTHON_ENV_PATH}/bin/python" "${IDF_PATH}/tools/idf.py")

# target | extra sdkconfig layer | label
configs=(
    "esp32c6||c6-internal"
    "esp32c5|sdkconfig.psram|c5-psram"
    "esp32c5|sdkconfig.internal|c5-psram-alloc-internal"
)

failed=0
mkdir -p build
for c in "${configs[@]}"; do
    IFS='|' read -r target layer label <<< "$c"
    defaults="sdkconfig.defaults${layer:+;$layer}"
    echo "=== $label ($target, $defaults)"
    if "${IDF_PY[@]}" -B "build/$label" -D SDKCONFIG="build/$label/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" \
        -D IDF_TARGET="$target" build > "build/$label.log" 2>&1; then
        echo "    OK"
    else
        echo "    FAILED (see test/build/build/$label.log)"
        failed=1
    fi
done
exit $failed
