#!/usr/bin/env bash
# Start the OpenEMS Strategy Engine (Linux / macOS / WSL)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SHM_NAME="${OPENEMS_SHM_NAME:-/openems_rt_db}"

export LD_LIBRARY_PATH="${APP_ROOT}/install/lib:${LD_LIBRARY_PATH:-}"

if [ -z "${OPENEMS_DB_URL:-}" ]; then
  echo "ERROR: OPENEMS_DB_URL is not set."
  exit 1
fi

echo "[strategy-engine] Starting with shm=$SHM_NAME"
exec "${APP_ROOT}/install/bin/openems-strategy-engine" "$SHM_NAME"
