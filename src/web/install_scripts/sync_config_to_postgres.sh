#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

if [ -z "${OPENEMS_DB_URL:-}" ]; then
  OPENEMS_DB_URL="postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
  export OPENEMS_DB_URL
fi

python_bin="python3"
if [ -x "$script_dir/.venv/bin/python" ]; then
  python_bin="$script_dir/.venv/bin/python"
elif [ -x "$(dirname "$script_dir")/.venv/bin/python" ]; then
  python_bin="$(dirname "$script_dir")/.venv/bin/python"
fi

echo "Working directory: $script_dir"
echo "OPENEMS_DB_URL is configured."
echo "Importing config/tables to structured PostgreSQL tables..."

exec "$python_bin" "./web/sync_config_to_postgres.py" --mode "import" --config-dir "config/tables"
