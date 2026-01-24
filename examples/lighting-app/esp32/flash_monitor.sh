#!/bin/bash

# Script to flash and monitor ESP32-C6 lighting-app firmware
# Usage: ./flash_monitor.sh [PORT]

set -e

PORT="${1:-/dev/cu.usbmodem2101}"

echo "Activating ESP-IDF environment..."

# Set Python 3.11 environment
export IDF_PYTHON_ENV_PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env"
export PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env/bin:$PATH"

cd ~/esp-idf
. ./export.sh

echo "Flashing and monitoring ESP32-C6 on port: $PORT"
cd /Users/austinkurpuis/connectedhomeip/examples/lighting-app/esp32

idf.py -p "$PORT" flash monitor
