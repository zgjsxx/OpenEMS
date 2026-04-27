#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

echo "Working directory: $script_dir"
echo "Starting OpenEMS Modbus collector..."

exec "./bin/openems-modbus-collector" "config/tables"
