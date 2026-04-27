#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

echo "Working directory: $script_dir"
echo "Starting OpenEMS RtDb service..."

exec "./bin/openems-rtdb-service" "config/tables"
