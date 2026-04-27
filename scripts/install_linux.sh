#!/usr/bin/env bash
# Install OpenEMS on Linux (from existing build)
# Must run after build_linux.sh
# Usage: ./scripts/install_linux.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
INSTALL_DIR="$PROJECT_ROOT/install"

echo "=== OpenEMS Linux Install ==="

cmake --install "$BUILD_DIR" --prefix "$INSTALL_DIR"

echo "=== Install complete: $INSTALL_DIR ==="