#!/bin/bash

# Script to erase flash on ESP32-C6 (factory reset)
# Usage: ./erase_flash.sh [PORT]

set -e

PORT="${1:-/dev/cu.usbmodem2101}"

echo "Erasing flash on ESP32-C6 (factory reset)..."

# Set Python 3.11 environment
export IDF_PYTHON_ENV_PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env"
export PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env/bin:$PATH"

cd ~/esp-idf
. ./export.sh

cd /Users/austinkurpuis/connectedhomeip/examples/lighting-app/esp32

echo "Erasing flash on port: $PORT"
idf.py -p "$PORT" erase-flash

echo ""
echo "Flash erased! Device has been factory reset."
echo ""
echo "Now reflash the firmware with:"
echo "  bash flash_monitor.sh $PORT"
