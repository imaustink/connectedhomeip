#!/bin/bash

# Script to flash ESP32-C6 lighting-app firmware
# Usage: ./flash_esp32c6.sh [PORT]
# Example: ./flash_esp32c6.sh /dev/ttyUSB0

set -e

# Get the port from argument or use default
PORT="${1:-/dev/cu.usbserial-0001}"

echo "Activating ESP-IDF environment..."

# Set Python environment BEFORE sourcing export.sh
export IDF_PYTHON_ENV_PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env"
export PATH="/Users/austinkurpuis/.espressif/python_env/idf5.5_py3.11_env/bin:$PATH"

cd ~/esp-idf
. ./export.sh

echo "Flashing ESP32-C6 on port: $PORT"
cd /Users/austinkurpuis/connectedhomeip/examples/lighting-app/esp32

# Flash the firmware
idf.py -p "$PORT" flash

echo ""
echo "Flash complete! To monitor serial output, run:"
echo "  idf.py -p $PORT monitor"
echo ""
echo "Or flash and monitor together:"
echo "  idf.py -p $PORT flash monitor"
