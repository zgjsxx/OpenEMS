#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

echo "Working directory: $script_dir"
if [ -z "${OPENEMS_DB_URL:-}" ]; then
  OPENEMS_DB_URL="postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
  export OPENEMS_DB_URL
fi

if [ -d "$script_dir/lib" ]; then
  LD_LIBRARY_PATH="$script_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export LD_LIBRARY_PATH
fi

echo "OPENEMS_DB_URL is configured."
echo "Starting OpenEMS RtDb service with PostgreSQL config source..."

exec "./bin/openems-rtdb-service" "postgresql"
