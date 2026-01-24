#!/bin/bash

# Script to regenerate ZAP artifacts for the lighting-app
# Usage: bash generate_zap.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIP_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

echo "Regenerating ZAP artifacts..."
echo "ZAP file: $SCRIPT_DIR/data_model/lighting-app.zap"

cd "$SCRIPT_DIR"

python3 "$CHIP_ROOT/scripts/tools/zap/generate.py" \
    "$SCRIPT_DIR/data_model/lighting-app.zap"

echo ""
echo "ZAP generation complete!"
echo "Generated files are in the build output during compilation."
