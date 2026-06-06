#!/bin/bash
# Exit immediately if any command fails
set -e

# Resolve directories relative to this script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building Project Horus ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Run cmake configuration
cmake ..

# Run compilation in parallel using available cores
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
make -j"$CORES"

echo "=== Launching Project Horus ==="
./Tactileviewer.app/Contents/MacOS/Tactileviewer
