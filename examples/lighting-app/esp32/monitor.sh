#!/bin/bash

# Script to monitor ESP32-C6 serial output
# Usage: ./monitor_esp32c6.sh [PORT]

set -e

PORT="${1:-/dev/cu.usbmodem2101}"

echo "Activating ESP-IDF environment..."

export IDF_PYTHON_ENV_PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env"
export PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env/bin:$PATH"

cd ~/esp-idf
. ./export.sh

echo "Monitoring ESP32-C6 on port: $PORT"
echo "Press Ctrl+] to exit"
cd /Users/austinkurpuis/connectedhomeip/examples/lighting-app/esp32

idf.py -p "$PORT" monitor