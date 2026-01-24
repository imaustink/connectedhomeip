#!/bin/bash

cd /Users/austinkurpuis/connectedhomeip

echo "=== Sourcing activate.sh ==="
. .environment/activate.sh 2>&1 | grep -v "pw: command not found" || true

echo "=== Checking environment variables ==="
echo "_PW_ACTUAL_ENVIRONMENT_ROOT=$_PW_ACTUAL_ENVIRONMENT_ROOT"
echo "PW_PROJECT_ROOT=$PW_PROJECT_ROOT"
echo "PW_ROOT=$PW_ROOT"

if [ -z "$_PW_ACTUAL_ENVIRONMENT_ROOT" ]; then
    echo "ERROR: _PW_ACTUAL_ENVIRONMENT_ROOT not set!"
    echo "Setting manually..."
    export _PW_ACTUAL_ENVIRONMENT_ROOT="/Users/austinkurpuis/connectedhomeip/.environment"
    export PW_PROJECT_ROOT="/Users/austinkurpuis/connectedhomeip"
    export PW_ROOT="/Users/austinkurpuis/connectedhomeip/third_party/pigweed/repo"
fi

# Add scripts directory to PYTHONPATH for python_path module
export PYTHONPATH="/Users/austinkurpuis/connectedhomeip/scripts:${PYTHONPATH:-}"
echo "PYTHONPATH=$PYTHONPATH"

# Add pigweed-venv to PATH and PYTHONPATH
if [ -d ".environment/pigweed-venv" ]; then
    echo "=== Configuring pigweed-venv ==="
    export PATH="/Users/austinkurpuis/connectedhomeip/.environment/pigweed-venv/bin:$PATH"
    export PYTHONPATH="/Users/austinkurpuis/connectedhomeip/.environment/pigweed-venv/lib/python3.11/site-packages:$PYTHONPATH"
    echo "Using Python: $(which python3)"
fi

set -e

echo "=== Running GN generation (this will take several minutes) ==="
.environment/cipd/packages/pigweed/gn gen --check --root=. build/chip --args='chip_crypto="boringssl"'

echo "=== Building chip_codegen_cmake target with Ninja ==="
.environment/cipd/packages/pigweed/ninja -C build/chip build/chip:chip_codegen_cmake

echo "=== Build complete! ==="
ls -lh build/chip/*.cmake 2>/dev/null || echo "No .cmake files found yet"
