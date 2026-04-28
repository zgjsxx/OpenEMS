#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

if [ -z "${OPENEMS_DB_URL:-}" ]; then
  OPENEMS_DB_URL="postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
  export OPENEMS_DB_URL
fi

echo "Working directory: $script_dir"
echo "OPENEMS_DB_URL is configured."
echo "Starting OpenEMS IEC104 collector..."

exec "./bin/openems-iec104-collector"
