#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="/opt/openems/install"
WEB_PORT="${OPENEMS_WEB_PORT:-8080}"
SYNC_ON_START="${OPENEMS_SYNC_CONFIG_ON_START:-1}"
SHM_NAME="/openems_rt_db"

cd "$APP_ROOT"

log() {
  printf '[docker-entrypoint] %s\n' "$*"
}

wait_for_postgres() {
  local retries=60
  while (( retries > 0 )); do
    if pg_isready -d "${OPENEMS_DB_URL}" >/dev/null 2>&1; then
      return 0
    fi
    retries=$((retries - 1))
    sleep 2
  done
  return 1
}

bootstrap_db() {
  log "Initializing PostgreSQL schema and default admin..."
  if [ "$SYNC_ON_START" = "1" ]; then
    python3 ./web/bootstrap_db.py --config-dir ./config/tables --sync-if-empty
  else
    python3 ./web/bootstrap_db.py --config-dir ./config/tables
  fi
}

declare -a PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  wait || true
}

trap cleanup EXIT INT TERM

LOG_DIR="${APP_ROOT}/runtime/logs"

start_service() {
  local name="$1"
  shift
  mkdir -p "$LOG_DIR"
  (
    "$@" 2>&1 | tee "$LOG_DIR/${name}.log" | sed -u "s/^/[$name] /"
  ) &
  PIDS+=("$!")
}

log "Working directory: $APP_ROOT"
log "Waiting for PostgreSQL..."
wait_for_postgres || { log "PostgreSQL is not ready."; exit 1; }

bootstrap_db

log "Starting OpenEMS RtDb service..."
start_service rtdb ./bin/openems-rtdb-service postgresql
sleep 2

if [ "${OPENEMS_ENABLE_MODBUS:-1}" = "1" ]; then
  log "Starting OpenEMS Modbus collector..."
  start_service modbus ./bin/openems-modbus-collector
fi

if [ "${OPENEMS_ENABLE_IEC104:-1}" = "1" ]; then
  log "Starting OpenEMS IEC104 collector..."
  start_service iec104 ./bin/openems-iec104-collector
fi

if [ "${OPENEMS_ENABLE_HISTORY:-1}" = "1" ]; then
  log "Starting OpenEMS history service..."
  start_service history ./bin/openems-history "$SHM_NAME" 1000
fi

if [ "${OPENEMS_ENABLE_ALARM:-1}" = "1" ]; then
  log "Starting OpenEMS alarm service..."
  start_service alarm ./bin/openems-alarm "$SHM_NAME"
fi

if [ "${OPENEMS_ENABLE_STRATEGY:-0}" = "1" ]; then
  log "Starting OpenEMS strategy engine..."
  start_service strategy ./bin/openems-strategy-engine "$SHM_NAME"
fi

log "Starting OpenEMS admin portal on port $WEB_PORT..."
start_service web python3 ./web/run_dashboard.py --port "$WEB_PORT"

wait -n "${PIDS[@]}"
exit 1
