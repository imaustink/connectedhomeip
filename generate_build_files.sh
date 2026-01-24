#!/bin/bash
cd "$(dirname "$0")"

echo "==> Activating Matter environment..."
source scripts/activate.sh 2>&1 | grep -v "pw: command not found" || true

echo "==> Generating build configuration..."
.environment/cipd/packages/pigweed/gn gen --check --root=. build/chip --args='chip_crypto="boringssl"'

echo "==> Building codegen cmake files..."
.environment/cipd/packages/pigweed/ninja -C build/chip build/chip:chip_codegen_cmake

echo "==> Done! Build files generated in build/chip/"
ls -lh build/chip/*.cmake 2>/dev/null || echo "Note: CMake files should be generated during ESP-IDF build"
