#!/bin/bash

cd "$(dirname "$0")"

echo "Activating ESP-IDF environment first..."
export IDF_PYTHON_ENV_PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env"
source /Users/austinkurpuis/esp-idf/export.sh

echo "Adding Matter environment to PATH..."
export CHIP_ROOT="/Users/austinkurpuis/connectedhomeip"
export _PW_ACTUAL_ENVIRONMENT_ROOT="/Users/austinkurpuis/connectedhomeip/.environment"
export PW_PROJECT_ROOT="/Users/austinkurpuis/connectedhomeip"
export PW_ROOT="/Users/austinkurpuis/connectedhomeip/third_party/pigweed/repo"

# Add Matter environment to PATH (append, not prepend)
export PATH="$PATH:/Users/austinkurpuis/connectedhomeip/.environment/cipd"
export PATH="$PATH:/Users/austinkurpuis/connectedhomeip/.environment/cipd/packages/pigweed"
export PATH="$PATH:/Users/austinkurpuis/connectedhomeip/.environment/cipd/packages/zap"

# Add Python paths (only scripts directory)
export PYTHONPATH="/Users/austinkurpuis/connectedhomeip/scripts:${PYTHONPATH:-}"

echo "Building ESP32-C6 lighting-app with Thread support..."
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6_thread" build

if [ $? -eq 0 ]; then
    echo "Build complete!"
    echo "Binary location: build/chip-lighting-app.bin"
else
    echo "Build failed!"
    exit 1
fi