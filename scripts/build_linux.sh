#!/usr/bin/env bash
# Build OpenEMS on Linux (Release mode)
# Usage: ./scripts/build_linux.sh [--debug] [--clean]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

BUILD_TYPE="Release"
CLEAN=false

for arg in "$@"; do
    case "$arg" in
        --debug)  BUILD_TYPE="Debug" ;;
        --clean)  CLEAN=true ;;
        *)        echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

if $CLEAN; then
    echo "Cleaning build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "=== OpenEMS Linux Build (type=$BUILD_TYPE) ==="

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "$BUILD_DIR" -j$(nproc)

echo "=== Build complete ==="