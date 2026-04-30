#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="/opt/openems/install"
WEB_PORT="${OPENEMS_WEB_PORT:-8080}"
SYNC_ON_START="${OPENEMS_SYNC_CONFIG_ON_START:-1}"
FORCE_SYNC_ON_START="${OPENEMS_FORCE_SYNC_CONFIG_ON_START:-0}"
SHM_NAME="${OPENEMS_SHM_NAME:-/openems_rt_db}"
LOG_DIR="${APP_ROOT}/runtime/logs"
PID_DIR="${APP_ROOT}/runtime/pids"
BOOTSTRAP_CONFIG_DIR="${OPENEMS_BOOTSTRAP_CONFIG_DIR:-./config/tables}"
INIT_SQL="${OPENEMS_INIT_SQL:-}"

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
  if [ "$FORCE_SYNC_ON_START" = "1" ]; then
    python3 ./web/bootstrap_db.py --config-dir "$BOOTSTRAP_CONFIG_DIR" --force-sync
  elif [ "$SYNC_ON_START" = "1" ]; then
    python3 ./web/bootstrap_db.py --config-dir "$BOOTSTRAP_CONFIG_DIR" --sync-if-empty
  else
    python3 ./web/bootstrap_db.py --config-dir "$BOOTSTRAP_CONFIG_DIR"
  fi
}

run_init_sql() {
  if [[ -z "$INIT_SQL" ]]; then
    return 0
  fi
  if [[ ! -f "$INIT_SQL" ]]; then
    log "Init SQL file not found: $INIT_SQL"
    return 1
  fi
  log "Applying init SQL: $INIT_SQL"
  psql "$OPENEMS_DB_URL" -v ON_ERROR_STOP=1 -f "$INIT_SQL"
}

pid_file() {
  printf '%s/%s.pid\n' "$PID_DIR" "$1"
}

log_file() {
  printf '%s/%s.log\n' "$LOG_DIR" "$1"
}

service_running() {
  local name="$1"
  local pid_path
  pid_path="$(pid_file "$name")"
  if [[ ! -f "$pid_path" ]]; then
    return 1
  fi
  local pid
  pid="$(cat "$pid_path" 2>/dev/null || true)"
  [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1
}

start_service() {
  local name="$1"
  shift
  mkdir -p "$LOG_DIR" "$PID_DIR"
  touch "$(log_file "$name")"
  if service_running "$name"; then
    log "Service $name is already running."
    return 0
  fi
  log "Starting service $name..."
  (
    cd "$APP_ROOT"
    nohup "$@" >>"$(log_file "$name")" 2>&1 &
    echo $! >"$(pid_file "$name")"
  )
}

stop_all_services() {
  if [[ ! -d "$PID_DIR" ]]; then
    return 0
  fi
  for pid_path in "$PID_DIR"/*.pid; do
    [[ -f "$pid_path" ]] || continue
    local pid
    pid="$(cat "$pid_path" 2>/dev/null || true)"
    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
    rm -f "$pid_path"
  done
}

TAIL_PID=""

cleanup() {
  if [[ -n "${TAIL_PID:-}" ]] && kill -0 "$TAIL_PID" >/dev/null 2>&1; then
    kill "$TAIL_PID" >/dev/null 2>&1 || true
  fi
  stop_all_services
  wait || true
}

trap cleanup EXIT INT TERM

log "Working directory: $APP_ROOT"
log "Waiting for PostgreSQL..."
wait_for_postgres || { log "PostgreSQL is not ready."; exit 1; }

bootstrap_db
run_init_sql

mkdir -p "$LOG_DIR" "$PID_DIR"
rm -f "$PID_DIR"/*.pid
touch "$(log_file rtdb)" "$(log_file modbus)" "$(log_file iec104)" "$(log_file history)" "$(log_file alarm)" "$(log_file strategy)" "$(log_file web)"

start_service rtdb ./bin/openems-rtdb-service postgresql
sleep 2

if [ "${OPENEMS_ENABLE_MODBUS:-1}" = "1" ]; then
  start_service modbus ./bin/openems-modbus-collector
fi

if [ "${OPENEMS_ENABLE_IEC104:-1}" = "1" ]; then
  start_service iec104 ./bin/openems-iec104-collector
fi

if [ "${OPENEMS_ENABLE_HISTORY:-1}" = "1" ]; then
  start_service history ./bin/openems-history "$SHM_NAME" 1000
fi

if [ "${OPENEMS_ENABLE_ALARM:-1}" = "1" ]; then
  start_service alarm ./bin/openems-alarm "$SHM_NAME"
fi

if [ "${OPENEMS_ENABLE_STRATEGY:-0}" = "1" ]; then
  start_service strategy ./bin/openems-strategy-engine "$SHM_NAME"
fi

start_service web python3 ./web/run_dashboard.py --port "$WEB_PORT"

tail -q -F "$LOG_DIR"/*.log &
TAIL_PID="$!"

while true; do
  sleep 3600
done
