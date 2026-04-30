"""OpenEMS single-site operations console with PostgreSQL-backed admin features."""

from __future__ import annotations

import asyncio
import csv
import logging
import os
import signal
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from auth import (
    SESSION_COOKIE,
    create_session_token,
    hash_password,
    hash_session_secret,
    split_session_token,
    verify_password,
)
from config_store import ConfigStore
from db import Database

try:
    from shm_reader import ShmReader
except Exception:
    class ShmReader:  # type: ignore[override]
        def __init__(self, shm_name: str = "Local\\openems_rt_db"):
            self.shm_name = shm_name
            self._attached = False

        def attach(self) -> bool:
            self._attached = False
            return False

        def detach(self):
            self._attached = False

        def is_attached(self) -> bool:
            return False

        def read_snapshot(self) -> Dict[str, Any]:
            return {"error": "Shared memory reader is not available on this platform.", "points": []}

        def submit_command(self, point_id: str, desired_value: float) -> Dict[str, Any]:
            return {"error": "Shared memory command submission is not available on this platform."}

        def read_command_status(self, point_id: str) -> Dict[str, Any]:
            return {"error": "Shared memory command status is not available on this platform."}

WEB_DIR = Path(__file__).resolve().parent
APP_ROOT = Path.cwd().resolve()
RUNTIME_DIR = APP_ROOT / "runtime"
CONFIG_DIR = APP_ROOT / "config" / "tables"
MIGRATIONS_DIR = WEB_DIR / "migrations"

ROLE_LEVELS = {"viewer": 1, "operator": 2, "admin": 3}

app = FastAPI(title="OpenEMS Operations Console")
app.mount("/assets", StaticFiles(directory=str(WEB_DIR / "assets")), name="assets")

_reader = ShmReader()
_config_store = ConfigStore(CONFIG_DIR, backup_root=RUNTIME_DIR / "config_backups")
_db = Database(MIGRATIONS_DIR)
_db_state: Dict[str, Any] = {"ok": False, "error": "Database not initialized."}
_system_metrics_cache: Dict[str, Any] = {
    "cpu_total": None,
    "cpu_idle": None,
    "net_rx_bytes": None,
    "net_tx_bytes": None,
    "ts": None,
}


class LoginRequest(BaseModel):
    username: str
    password: str


class CommandRequest(BaseModel):
    point_id: str
    desired_value: float


class ConfigEditorRequest(BaseModel):
    tables: Dict[str, List[Dict[str, Any]]]


class UserCreateRequest(BaseModel):
    username: str
    password: str
    role: str
    status: str = "active"


class UserUpdateRequest(BaseModel):
    role: Optional[str] = None
    status: Optional[str] = None
    password: Optional[str] = None


POINT_TABLES = {
    "telemetry": "telemetry.csv",
    "teleindication": "teleindication.csv",
    "telecontrol": "telecontrol.csv",
    "teleadjust": "teleadjust.csv",
}

LOG_DIR = RUNTIME_DIR / "logs"
PID_DIR = RUNTIME_DIR / "pids"

SERVICE_SPECS: Dict[str, Dict[str, Any]] = {
    "rtdb": {
        "display_name": "openems-rtdb-service",
        "command": ["./bin/openems-rtdb-service", "postgresql"],
        "controllable": True,
    },
    "modbus": {
        "display_name": "openems-modbus-collector",
        "command": ["./bin/openems-modbus-collector"],
        "controllable": True,
    },
    "iec104": {
        "display_name": "openems-iec104-collector",
        "command": ["./bin/openems-iec104-collector"],
        "controllable": True,
    },
    "history": {
        "display_name": "openems-history",
        "command": ["./bin/openems-history", "/openems_rt_db", "1000"],
        "controllable": True,
    },
    "alarm": {
        "display_name": "openems-alarm",
        "command": ["./bin/openems-alarm", "/openems_rt_db"],
        "controllable": True,
    },
    "strategy": {
        "display_name": "openems-strategy-engine",
        "command": ["./bin/openems-strategy-engine", "/openems_rt_db"],
        "controllable": True,
    },
    "web": {
        "display_name": "openems-admin-portal",
        "command": ["python3", "./web/run_dashboard.py", "--port", os.environ.get("OPENEMS_WEB_PORT", "8080")],
        "controllable": False,
        "note": "当前后台进程不支持在页面内停止或重启自身。",
    },
}


def _model_dump(model: BaseModel) -> Dict[str, Any]:
    if hasattr(model, "model_dump"):
        return model.model_dump()
    return model.dict()


def _client_ip(request: Request) -> str:
    forwarded = request.headers.get("x-forwarded-for", "")
    if forwarded:
        return forwarded.split(",")[0].strip()
    if request.client:
        return request.client.host
    return ""


def _json_error(message: str, status_code: int) -> JSONResponse:
    return JSONResponse({"error": message}, status_code=status_code)


def _db_ready() -> bool:
    return bool(_db_state.get("ok") and _db.available)


def _require_db() -> None:
    if _db_ready():
        return
    raise HTTPException(status_code=503, detail=_db_state.get("error") or _db.last_error or "Database unavailable.")


def _role_ok(actual: str, required: str) -> bool:
    return ROLE_LEVELS.get(actual or "", 0) >= ROLE_LEVELS.get(required, 0)


def _session_user(request: Request, required_role: str = "viewer") -> Dict[str, Any]:
    _require_db()
    cookie = request.cookies.get(SESSION_COOKIE)
    if not cookie:
        raise HTTPException(status_code=401, detail="Login required.")

    try:
        session_id, secret = split_session_token(cookie)
    except ValueError:
        raise HTTPException(status_code=401, detail="Invalid session.")

    session = _db.get_session(session_id)
    if not session:
        raise HTTPException(status_code=401, detail="Session not found.")
    if session.get("revoked_at"):
        raise HTTPException(status_code=401, detail="Session revoked.")

    expires_at = session.get("expires_at")
    if isinstance(expires_at, datetime):
        current_utc = datetime.now(timezone.utc)
        if expires_at.tzinfo is None:
            expires_at = expires_at.replace(tzinfo=timezone.utc)
        if expires_at <= current_utc:
            raise HTTPException(status_code=401, detail="Session expired.")

    if session.get("status") != "active":
        raise HTTPException(status_code=403, detail="User is disabled.")
    if hash_session_secret(secret) != session.get("token_hash"):
        raise HTTPException(status_code=401, detail="Session validation failed.")
    if not _role_ok(str(session.get("role")), required_role):
        raise HTTPException(status_code=403, detail="Permission denied.")

    _db.touch_session(session_id)
    return {
        "id": session.get("user_id"),
        "username": session.get("username"),
        "role": session.get("role"),
        "status": session.get("status"),
        "session_id": session_id,
    }


def _page_response(request: Request, filename: str, required_role: str = "viewer") -> HTMLResponse:
    try:
        _session_user(request, required_role)
    except HTTPException:
        return RedirectResponse(url="/login", status_code=303)
    return HTMLResponse((WEB_DIR / filename).read_text(encoding="utf-8"))


logger = logging.getLogger("openems.admin")
_ingest_logger = logging.getLogger("openems.ingestion")
def _point_lookup() -> Dict[str, Dict[str, Any]]:
    _require_db()
    lookup: Dict[str, Dict[str, Any]] = {}
    for row in _db.list_point_metadata():
        point_id = str(row.get("id") or "").strip()
        if not point_id:
            continue
        lookup[point_id] = row
    return lookup

def _active_alarm_payload() -> Dict[str, Any]:
    _require_db()
    rows = _db.list_alarms(status="active", limit=1000)
    alarms: List[Dict[str, Any]] = []
    generated_at = 0
    for row in rows:
        ts_text = row.get("last_seen_at") or row.get("active_since")
        ts_ms = 0
        if isinstance(ts_text, str) and ts_text:
            try:
                ts_ms = int(datetime.fromisoformat(ts_text.replace("Z", "+00:00")).timestamp() * 1000)
            except ValueError:
                ts_ms = 0
        generated_at = max(generated_at, ts_ms)
        alarms.append(
            {
                "id": row.get("alarm_id", ""),
                "alarm_id": row.get("alarm_id", ""),
                "severity": row.get("severity", ""),
                "level": row.get("severity", ""),
                "point_id": row.get("point_id", ""),
                "device_id": row.get("device_id", ""),
                "message": row.get("message", ""),
                "value_display": row.get("value_text", ""),
                "trigger_time": ts_ms,
                "last_update_time": ts_ms,
                "status": row.get("status", ""),
            }
        )
    return {"generated_at": generated_at, "count": len(alarms), "alarms": alarms}


def _read_point_metadata() -> List[Dict[str, Any]]:
    return list(_point_lookup().values())


def _config_overview() -> Dict[str, Any]:
    data, source, warning = _load_config_tables()
    devices = data.get("device", [])
    device_counts: Dict[str, int] = {}
    protocol_counts: Dict[str, int] = {}
    for device in devices:
        device_counts[device.get("type") or "Unknown"] = device_counts.get(device.get("type") or "Unknown", 0) + 1
        protocol_counts[device.get("protocol") or "Unknown"] = protocol_counts.get(device.get("protocol") or "Unknown", 0) + 1

    point_counts = {name: len(data.get(name, [])) for name in POINT_TABLES}
    mapping_counts = {"modbus_mapping": len(data.get("modbus_mapping", [])), "iec104_mapping": len(data.get("iec104_mapping", []))}
    return {
        "device_count": len(devices),
        "device_type_counts": device_counts,
        "protocol_counts": protocol_counts,
        "point_counts": point_counts,
        "mapping_counts": mapping_counts,
        "tables": data,
        "source": source,
        "warning": warning,
    }


def _load_config_tables() -> tuple[Dict[str, Any], str, str | None]:
    _require_db()
    return _db.load_structured_config(), "postgresql", None


def _parse_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _point_value_lookup(snapshot: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    points: Dict[str, Dict[str, Any]] = {}
    for point in snapshot.get("points", []) or []:
        point_id = str(point.get("id") or "")
        if not point_id:
            continue
        value = point.get("value")
        if value is None and "state_code" in point:
            value = point.get("state_code")
        enriched = dict(point)
        enriched["value"] = value
        points[point_id] = enriched
    return points


def _alarm_lookup(alarms: List[Dict[str, Any]]) -> Dict[str, List[Dict[str, Any]]]:
    by_point: Dict[str, List[Dict[str, Any]]] = {}
    for alarm in alarms:
        point_id = str(alarm.get("point_id") or "")
        if not point_id:
            continue
        by_point.setdefault(point_id, []).append(alarm)
    return by_point


def _highest_severity(alarms: List[Dict[str, Any]]) -> str:
    rank = {"info": 1, "warning": 2, "critical": 3}
    highest = ""
    for alarm in alarms:
        severity = str(alarm.get("severity") or "").lower()
        if rank.get(severity, 0) > rank.get(highest, 0):
            highest = severity
    return highest


def _build_topology_payload(tables: Dict[str, List[Dict[str, Any]]], snapshot: Dict[str, Any], alarm_payload: Dict[str, Any]) -> Dict[str, Any]:
    devices = {row.get("id"): row for row in tables.get("device", []) if row.get("id")}
    point_meta = _point_lookup()
    point_values = _point_value_lookup(snapshot)
    alarms = alarm_payload.get("alarms", []) or []
    alarms_by_point = _alarm_lookup(alarms)

    nodes = []
    node_by_id: Dict[str, Dict[str, Any]] = {}
    for row in tables.get("topology_node", []):
        enabled = str(row.get("enabled") or "").lower() == "true"
        device_id = str(row.get("device_id") or "")
        node = {
            "id": row.get("id", ""),
            "site_id": row.get("site_id", ""),
            "name": row.get("name", ""),
            "type": row.get("type", "unknown"),
            "device_id": device_id,
            "device": devices.get(device_id) if device_id else None,
            "device_enabled": bool(device_id and device_id in devices),
            "x": _parse_float(row.get("x")),
            "y": _parse_float(row.get("y")),
            "enabled": enabled,
            "status": "disabled" if not enabled else "normal",
            "severity": "",
            "binding_count": 0,
            "no_data_count": 0,
            "alarm_count": 0,
        }
        nodes.append(node)
        node_by_id[str(node["id"])] = node

    links = []
    link_by_id: Dict[str, Dict[str, Any]] = {}
    for row in tables.get("topology_link", []):
        enabled = str(row.get("enabled") or "").lower() == "true"
        link = {
            "id": row.get("id", ""),
            "site_id": row.get("site_id", ""),
            "source_node_id": row.get("source_node_id", ""),
            "target_node_id": row.get("target_node_id", ""),
            "type": row.get("type", "line"),
            "enabled": enabled,
            "status": "disabled" if not enabled else "normal",
            "severity": "",
            "binding_count": 0,
            "no_data_count": 0,
            "alarm_count": 0,
        }
        links.append(link)
        link_by_id[str(link["id"])] = link

    bindings = []
    total_no_data = 0
    for row in tables.get("topology_binding", []):
        point_id = str(row.get("point_id") or "")
        point = point_values.get(point_id)
        meta = point_meta.get(point_id, {})
        point_alarms = alarms_by_point.get(point_id, [])
        valid = bool(point and point.get("valid"))
        binding = {
            "id": row.get("id", ""),
            "target_type": row.get("target_type", ""),
            "target_id": row.get("target_id", ""),
            "point_id": point_id,
            "role": row.get("role", ""),
            "label": row.get("label") or meta.get("name") or point_id,
            "point": point,
            "point_meta": meta,
            "valid": valid,
            "device_enabled": bool(meta.get("device_id") in devices) if meta.get("device_id") else False,
            "alarms": point_alarms,
            "severity": _highest_severity(point_alarms),
        }
        bindings.append(binding)

        target_lookup = node_by_id if binding["target_type"] == "node" else link_by_id
        target = target_lookup.get(str(binding["target_id"]))
        if not target:
            continue
        target["binding_count"] += 1
        if point_alarms:
            target["alarm_count"] += len(point_alarms)
            severity = binding["severity"]
            if severity == "critical" or (severity == "warning" and target.get("severity") != "critical"):
                target["severity"] = severity
                target["status"] = "alarm"
        if not valid:
            target["no_data_count"] += 1
            total_no_data += 1
            if target["status"] == "normal":
                target["status"] = "no-data"
        if meta.get("device_id") and meta.get("device_id") not in devices and target["status"] == "normal":
            target["status"] = "config-warning"

    return {
        "generated_at": int(time.time() * 1000),
        "snapshot": {
            "available": "error" not in snapshot,
            "error": snapshot.get("error", ""),
            "last_update_time": snapshot.get("last_update_time"),
            "last_update_str": snapshot.get("last_update_str", "-"),
            "update_seq": snapshot.get("update_seq"),
        },
        "summary": {
            "node_count": len(nodes),
            "link_count": len(links),
            "binding_count": len(bindings),
            "alarm_count": len(alarms),
            "no_data_count": total_no_data,
        },
        "nodes": nodes,
        "links": links,
        "bindings": bindings,
        "alarms": alarms,
    }


def _safe_audit(
    request: Request,
    *,
    action: str,
    resource_type: str,
    resource_id: str,
    result: str,
    before_json: Optional[Dict[str, Any]] = None,
    after_json: Optional[Dict[str, Any]] = None,
    details: str = "",
    user: Optional[Dict[str, Any]] = None,
) -> None:
    if not _db_ready():
        return
    actor = user or {"id": None, "username": "system"}
    _db.log_audit(
        user_id=actor.get("id"),
        username=str(actor.get("username") or "system"),
        action=action,
        resource_type=resource_type,
        resource_id=resource_id,
        result=result,
        client_ip=_client_ip(request),
        before_json=before_json,
        after_json=after_json,
        details=details,
    )


def _write_manual_override_for_point(point_id: str, duration_minutes: int = 30) -> None:
    """When a user manually sends a command to a point, mark all strategies
    targeting that point's device as manually overridden for the given duration."""
    if not _db_ready():
        return
    try:
        point_row = _db.fetch_one(
            "SELECT device_id FROM points WHERE id = %s", (point_id,)
        )
        if not point_row:
            return
        device_id = point_row.get("device_id")
        if not device_id:
            return
        strategies = _db.fetch_all(
            """
            SELECT DISTINCT sd.id
            FROM strategy_definitions sd
            LEFT JOIN strategy_bindings sb ON sb.strategy_id = sd.id
            WHERE sd.enabled = TRUE
              AND (sd.device_id = %s OR sb.point_id = %s)
            """,
            (device_id, point_id),
        )
        for s in strategies:
            _db.execute(
                "INSERT INTO strategy_runtime_state(strategy_id, manual_override_until) "
                "VALUES (%s, NOW() + INTERVAL '%s minutes') "
                "ON CONFLICT (strategy_id) DO UPDATE SET "
                "manual_override_until = NOW() + INTERVAL '%s minutes', "
                "updated_at = NOW()",
                (s["id"], str(duration_minutes), str(duration_minutes)),
            )
    except Exception:
        pass


@app.on_event("startup")
async def startup():
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    (RUNTIME_DIR / "config_backups").mkdir(parents=True, exist_ok=True)
    _reader.attach()
    global _db_state
    _db_state = _db.initialize()


@app.get("/", response_class=HTMLResponse)
async def root(request: Request):
    try:
        _session_user(request)
        return RedirectResponse(url="/dashboard", status_code=303)
    except HTTPException:
        return RedirectResponse(url="/login", status_code=303)


@app.get("/login", response_class=HTMLResponse)
async def login_page():
    return HTMLResponse((WEB_DIR / "login.html").read_text(encoding="utf-8"))


@app.get("/dashboard", response_class=HTMLResponse)
async def dashboard_page(request: Request):
    return _page_response(request, "dashboard_admin.html")


@app.get("/alarms", response_class=HTMLResponse)
async def alarms_page(request: Request):
    return _page_response(request, "alarms_admin.html")


@app.get("/history", response_class=HTMLResponse)
async def history_page(request: Request):
    return _page_response(request, "history_admin.html")


@app.get("/config", response_class=HTMLResponse)
async def config_page(request: Request):
    return _page_response(request, "config_admin.html")


@app.get("/comm", response_class=HTMLResponse)
async def comm_page(request: Request):
    return _page_response(request, "comm_admin.html")


@app.get("/topology", response_class=HTMLResponse)
async def topology_page(request: Request):
    return _page_response(request, "topology_admin.html")


@app.get("/users", response_class=HTMLResponse)
async def users_page(request: Request):
    return _page_response(request, "users_admin.html", "admin")


@app.get("/audit", response_class=HTMLResponse)
async def audit_page(request: Request):
    return _page_response(request, "audit_admin.html", "admin")


@app.get("/api/auth/me")
async def api_auth_me(request: Request):
    user = _session_user(request, "viewer")
    return JSONResponse(
        {
            "user": {"id": user["id"], "username": user["username"], "role": user["role"], "status": user["status"]},
            "db": _db.health(),
        }
    )


@app.post("/api/auth/login")
async def api_auth_login(request: Request, req: LoginRequest):
    _require_db()
    username = req.username.strip()
    user = _db.get_user_by_username(username)
    if not user or not verify_password(req.password, str(user.get("password_hash") or "")):
        _safe_audit(request, action="login", resource_type="auth", resource_id=username, result="failed", details="Invalid username or password.")
        return _json_error("Invalid username or password.", 401)
    if user.get("status") != "active":
        _safe_audit(request, action="login", resource_type="auth", resource_id=username, result="failed", details="User is disabled.")
        return _json_error("User is disabled.", 403)

    session_id, cookie_value = create_session_token()
    _, secret = split_session_token(cookie_value)
    _db.create_session(session_id, hash_session_secret(secret), int(user["id"]), _client_ip(request), request.headers.get("user-agent", ""))
    _db.touch_login(int(user["id"]))
    _safe_audit(
        request,
        action="login",
        resource_type="auth",
        resource_id=username,
        result="success",
        after_json={"role": user.get("role")},
        user={"id": user.get("id"), "username": username},
    )

    response = JSONResponse({"ok": True, "user": {"id": user["id"], "username": username, "role": user["role"], "status": user["status"]}})
    response.set_cookie(SESSION_COOKIE, cookie_value, httponly=True, samesite="lax", max_age=12 * 3600)
    return response


@app.post("/api/auth/logout")
async def api_auth_logout(request: Request):
    user = _session_user(request, "viewer")
    cookie = request.cookies.get(SESSION_COOKIE, "")
    try:
        session_id, _ = split_session_token(cookie)
        _db.revoke_session(session_id)
    except Exception:
        pass
    _safe_audit(request, action="logout", resource_type="auth", resource_id=str(user["username"]), result="success", user=user)
    response = JSONResponse({"ok": True})
    response.delete_cookie(SESSION_COOKIE)
    return response


@app.get("/api/system/status")
async def api_system_status(request: Request):
    _session_user(request, "viewer")
    alarm_payload = _active_alarm_payload()
    config_summary = _config_overview()
    return JSONResponse(
        {
            "db": _db.health(),
            "alarms_active": len(alarm_payload.get("alarms", [])),
            "device_count": config_summary["device_count"],
            "protocol_counts": config_summary["protocol_counts"],
            "point_counts": config_summary["point_counts"],
        }
    )


@app.get("/api/snapshot")
async def api_snapshot(request: Request):
    _session_user(request, "viewer")
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if not _reader.is_attached():
        return JSONResponse({"error": "Shared memory not available. Is a collector running?"}, status_code=503)

    data = await asyncio.to_thread(_reader.read_snapshot)
    if "error" in data and data.get("points", []) == []:
        _reader.detach()
        return JSONResponse(data, status_code=503)
    return JSONResponse(data)


@app.post("/api/command")
async def api_submit_command(request: Request, req: CommandRequest):
    user = _session_user(request, "operator")
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if not _reader.is_attached():
        return JSONResponse({"error": "Shared memory not available"}, status_code=503)
    result = await asyncio.to_thread(_reader.submit_command, req.point_id, req.desired_value)
    if "error" in result:
        _safe_audit(
            request,
            action="command_submit",
            resource_type="point",
            resource_id=req.point_id,
            result="failed",
            after_json={"desired_value": req.desired_value},
            details=str(result.get("error")),
            user=user,
        )
        return JSONResponse(result, status_code=400)

    _safe_audit(
        request,
        action="command_submit",
        resource_type="point",
        resource_id=req.point_id,
        result="success",
        after_json={"desired_value": req.desired_value},
        user=user,
    )

    # Write manual override: strategies targeting this point's device pause for 30 min
    try:
        _write_manual_override_for_point(req.point_id)
    except Exception:
        pass

    return JSONResponse(result)


@app.get("/api/command/{point_id}")
async def api_command_status(request: Request, point_id: str):
    _session_user(request, "viewer")
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if not _reader.is_attached():
        return JSONResponse({"error": "Shared memory not available"}, status_code=503)
    result = await asyncio.to_thread(_reader.read_command_status, point_id)
    if "error" in result:
        return JSONResponse(result, status_code=404)
    return JSONResponse(result)


@app.get("/api/alarms/active")
async def api_active_alarms(request: Request):
    _session_user(request, "viewer")
    return JSONResponse(_active_alarm_payload())


@app.get("/api/alarms")
async def api_alarms(request: Request, status: str = "active", severity: str = "", device_id: str = "", limit: int = 200):
    _session_user(request, "viewer")
    _require_db()
    rows = _db.list_alarms(status=status or None, severity=severity or None, device_id=device_id or None, limit=limit)
    return JSONResponse({"rows": rows, "count": len(rows)})


@app.post("/api/alarms/{alarm_id}/ack")
async def api_alarm_ack(request: Request, alarm_id: str):
    user = _session_user(request, "operator")
    row = _db.ack_alarm(alarm_id, str(user["username"]))
    if not row:
        return _json_error("Alarm not found.", 404)
    _safe_audit(request, action="alarm_ack", resource_type="alarm", resource_id=alarm_id, result="success", after_json=row, user=user)
    return JSONResponse({"ok": True, "alarm": row})


@app.post("/api/alarms/{alarm_id}/silence")
async def api_alarm_silence(request: Request, alarm_id: str):
    user = _session_user(request, "operator")
    row = _db.silence_alarm(alarm_id, str(user["username"]))
    if not row:
        return _json_error("Alarm not found.", 404)
    _safe_audit(request, action="alarm_silence", resource_type="alarm", resource_id=alarm_id, result="success", after_json=row, user=user)
    return JSONResponse({"ok": True, "alarm": row})


@app.get("/api/history/points")
async def api_history_points(request: Request):
    _session_user(request, "viewer")
    _require_db()
    return JSONResponse({"points": _read_point_metadata()})


@app.get("/api/history/query")
async def api_history_query(request: Request, point_id: str, start: Optional[int] = None, end: Optional[int] = None, limit: int = 5000):
    _session_user(request, "viewer")
    _require_db()
    now = int(time.time() * 1000)
    end_ms = end if end is not None else now
    start_ms = start if start is not None else end_ms - 3600 * 1000
    if start_ms > end_ms:
        start_ms, end_ms = end_ms, start_ms

    limit = max(1, min(limit, 20000))

    rows = await asyncio.to_thread(_db.query_history, point_id, start_ms, end_ms, limit)
    return JSONResponse({"point_id": point_id, "count": len(rows), "rows": rows, "source": "timescaledb"})


@app.get("/api/history/query_multi")
async def api_history_query_multi(request: Request, point_ids: str, start: Optional[int] = None, end: Optional[int] = None, limit: int = 5000):
    _session_user(request, "viewer")
    _require_db()
    ids = [pid.strip() for pid in point_ids.split(",") if pid.strip()]
    if not ids:
        return _json_error("point_ids is required.", 400)
    if len(ids) > 8:
        return _json_error("Maximum 8 point_ids allowed.", 400)

    now = int(time.time() * 1000)
    end_ms = end if end is not None else now
    start_ms = start if start is not None else end_ms - 3600 * 1000
    if start_ms > end_ms:
        start_ms, end_ms = end_ms, start_ms
    limit = max(1, min(limit, 20000))

    series = await asyncio.to_thread(_db.query_history_multi, ids, start_ms, end_ms, limit)
    return JSONResponse({"point_ids": ids, "series": series, "source": "timescaledb"})


@app.get("/api/history/query_aggregate")
async def api_history_query_aggregate(request: Request, point_id: str, interval: str = "1h", start: Optional[int] = None, end: Optional[int] = None):
    _session_user(request, "viewer")
    _require_db()
    if interval not in ("1h", "1d", "1w", "1mo"):
        return _json_error("interval must be one of: 1h, 1d, 1w, 1mo.", 400)

    now = int(time.time() * 1000)
    end_ms = end if end is not None else now
    start_ms = start if start is not None else end_ms - 3600 * 1000
    if start_ms > end_ms:
        start_ms, end_ms = end_ms, start_ms

    result = await asyncio.to_thread(_db.query_history_aggregate, point_id, start_ms, end_ms, interval)
    return JSONResponse({**result, "source": "timescaledb"})


@app.get("/api/config")
async def api_config(request: Request):
    _session_user(request, "viewer")
    data = _config_overview()
    return JSONResponse(
        {
            "devices": data["tables"].get("device", []),
            "telemetry": data["tables"].get("telemetry", []),
            "teleindication": data["tables"].get("teleindication", []),
            "telecontrol": data["tables"].get("telecontrol", []),
            "teleadjust": data["tables"].get("teleadjust", []),
        }
    )


@app.get("/api/config/overview")
async def api_config_overview(request: Request):
    _session_user(request, "viewer")
    return JSONResponse(_config_overview())


@app.get("/api/topology")
async def api_topology(request: Request):
    _session_user(request, "viewer")
    tables, _, _ = await asyncio.to_thread(_load_config_tables)
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if _reader.is_attached():
        snapshot = await asyncio.to_thread(_reader.read_snapshot)
        if "error" in snapshot and snapshot.get("points", []) == []:
            _reader.detach()
    else:
        snapshot = {"error": "Shared memory not available. Is a collector running?", "points": []}
    alarm_payload = _active_alarm_payload()
    return JSONResponse(_build_topology_payload(tables, snapshot, alarm_payload))


@app.get("/api/comm/schema")
async def api_comm_schema(request: Request):
    _session_user(request, "viewer")
    return JSONResponse(_config_store.schema())


@app.get("/api/comm/data")
async def api_comm_data(request: Request):
    _session_user(request, "viewer")
    tables, source, warning = await asyncio.to_thread(_load_config_tables)
    payload = {"tables": tables, "source": source}
    if warning:
        payload["warning"] = warning
    return JSONResponse(payload)


@app.post("/api/comm/validate")
async def api_comm_validate(request: Request, req: ConfigEditorRequest):
    _session_user(request, "admin")
    result = await asyncio.to_thread(_config_store.validate, _model_dump(req))
    status_code = 200 if result["ok"] else 400
    return JSONResponse(result, status_code=status_code)


@app.post("/api/comm/save")
async def api_comm_save(request: Request, req: ConfigEditorRequest):
    user = _session_user(request, "admin")
    before_tables, _, _ = await asyncio.to_thread(_load_config_tables)
    result = await asyncio.to_thread(_config_store.validate, _model_dump(req))
    if result["ok"]:
        if not _db_ready():
            result = {
                "ok": False,
                "errors": [{
                    "table": "postgresql",
                    "row": None,
                    "column": None,
                    "message": _db_state.get("error") or _db.last_error or "Database unavailable.",
                }],
                "tables": result.get("tables", {}),
            }
        else:
            try:
                await asyncio.to_thread(_db.save_structured_config, result.get("tables", {}))
                result.update({
                    "ok": True,
                    "message": "Structured PostgreSQL config saved successfully.",
                    "source": "postgresql",
                    "restart_required": [
                        "openems-rtdb-service",
                        "openems-modbus-collector",
                        "openems-iec104-collector",
                    ],
                    "auto_reload": "openems-alarm (告警规则每30秒自动重载，无需重启)",
                })
            except Exception as exc:
                result = {
                    "ok": False,
                    "errors": [{
                        "table": "postgresql",
                        "row": None,
                        "column": None,
                        "message": "Structured config save failed: " + str(exc),
                    }],
                    "tables": result.get("tables", {}),
                }
    status_code = 200 if result["ok"] else 400
    _safe_audit(
        request,
        action="comm_save",
        resource_type="structured_config",
        resource_id="postgresql",
        result="success" if result["ok"] else "failed",
        before_json={"tables": before_tables},
        after_json={"tables": result.get("tables", {})},
        details=str(result.get("message") or ""),
        user=user,
    )
    return JSONResponse(result, status_code=status_code)


@app.get("/api/config-editor/schema")
async def api_config_editor_schema(request: Request):
    return await api_comm_schema(request)


@app.get("/api/config-editor/data")
async def api_config_editor_data(request: Request):
    return await api_comm_data(request)


@app.post("/api/config-editor/validate")
async def api_config_editor_validate(request: Request, req: ConfigEditorRequest):
    return await api_comm_validate(request, req)


@app.post("/api/config-editor/save")
async def api_config_editor_save(request: Request, req: ConfigEditorRequest):
    return await api_comm_save(request, req)


@app.get("/api/users")
async def api_users(request: Request):
    _session_user(request, "admin")
    _require_db()
    return JSONResponse({"rows": _db.list_users()})


@app.post("/api/users")
async def api_users_create(request: Request, req: UserCreateRequest):
    user = _session_user(request, "admin")
    _require_db()
    username = req.username.strip()
    if not username or not req.password:
        return _json_error("username and password are required.", 400)
    if req.role not in ROLE_LEVELS:
        return _json_error("Invalid role.", 400)
    if req.status not in {"active", "disabled"}:
        return _json_error("Invalid status.", 400)
    if _db.get_user_by_username(username):
        return _json_error("Username already exists.", 409)
    created = _db.create_user(username, hash_password(req.password), req.role, req.status)
    _safe_audit(request, action="user_create", resource_type="user", resource_id=username, result="success", after_json=created, user=user)
    return JSONResponse({"ok": True, "user": created})


@app.patch("/api/users/{user_id}")
async def api_users_patch(request: Request, user_id: int, req: UserUpdateRequest):
    user = _session_user(request, "admin")
    _require_db()
    before = _db.get_user_by_id(user_id)
    if not before:
        return _json_error("User not found.", 404)
    if req.role and req.role not in ROLE_LEVELS:
        return _json_error("Invalid role.", 400)
    if req.status and req.status not in {"active", "disabled"}:
        return _json_error("Invalid status.", 400)
    updated = _db.update_user(user_id, role=req.role, status=req.status, password_hash=hash_password(req.password) if req.password else None)
    if not updated:
        return _json_error("Failed to update user.", 400)
    _safe_audit(request, action="user_update", resource_type="user", resource_id=str(user_id), result="success", before_json=before, after_json=updated, user=user)
    return JSONResponse({"ok": True, "user": updated})


# ---- Strategy Management ----


class StrategySaveRequest(BaseModel):
    tables: Dict[str, Any]


@app.get("/strategy", response_class=HTMLResponse)
async def strategy_page(request: Request):
    return _page_response(request, "strategy_admin.html")


@app.get("/api/strategy")
async def api_strategy_list(request: Request):
    _session_user(request, "viewer")
    _require_db()
    rows = _db.fetch_all("""
        SELECT sd.id, sd.name, sd.type, sd.enabled, sd.site_id, sd.device_id,
               sd.priority, sd.cycle_ms
        FROM strategy_definitions sd
        ORDER BY sd.priority ASC, sd.id ASC
    """)
    for row in rows:
        row["enabled"] = bool(row.get("enabled"))
        bindings = _db.fetch_all(
            "SELECT role, point_id FROM strategy_bindings WHERE strategy_id = %s",
            (row["id"],),
        )
        row["bindings"] = [
            {"role": b["role"], "point_id": b["point_id"]} for b in bindings
        ]
        params = _db.fetch_all(
            "SELECT param_key, param_value FROM strategy_params WHERE strategy_id = %s",
            (row["id"],),
        )
        row["params"] = {p["param_key"]: p["param_value"] for p in params}
    return JSONResponse({"rows": rows, "count": len(rows)})


@app.post("/api/strategy/save")
async def api_strategy_save(request: Request, req: StrategySaveRequest):
    user = _session_user(request, "admin")
    _require_db()
    tables = req.tables
    errors = []
    try:
        # Save strategy_definitions
        for row in tables.get("strategy_definitions", []):
            sid = row.get("id", "")
            if not sid:
                errors.append("Missing strategy id")
                continue
            _db.execute("""
                INSERT INTO strategy_definitions(id, name, type, enabled, site_id, device_id, priority, cycle_ms)
                VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
                ON CONFLICT (id) DO UPDATE SET
                    name = EXCLUDED.name,
                    type = EXCLUDED.type,
                    enabled = EXCLUDED.enabled,
                    site_id = EXCLUDED.site_id,
                    device_id = EXCLUDED.device_id,
                    priority = EXCLUDED.priority,
                    cycle_ms = EXCLUDED.cycle_ms,
                    updated_at = NOW()
            """, (
                sid, row.get("name", ""), row.get("type", ""),
                row.get("enabled", True), row.get("site_id", ""),
                row.get("device_id", ""), row.get("priority", 0),
                row.get("cycle_ms", 1000),
            ))

        # Save bindings
        for row in tables.get("strategy_bindings", []):
            _db.execute("""
                INSERT INTO strategy_bindings(id, strategy_id, role, point_id)
                VALUES (%s, %s, %s, %s)
                ON CONFLICT (id) DO UPDATE SET
                    strategy_id = EXCLUDED.strategy_id,
                    role = EXCLUDED.role,
                    point_id = EXCLUDED.point_id
            """, (row.get("id", ""), row.get("strategy_id", ""),
                  row.get("role", ""), row.get("point_id", "")))

        # Save params
        for row in tables.get("strategy_params", []):
            _db.execute("""
                INSERT INTO strategy_params(id, strategy_id, param_key, param_value)
                VALUES (%s, %s, %s, %s)
                ON CONFLICT (id) DO UPDATE SET
                    strategy_id = EXCLUDED.strategy_id,
                    param_key = EXCLUDED.param_key,
                    param_value = EXCLUDED.param_value
            """, (row.get("id", ""), row.get("strategy_id", ""),
                  row.get("param_key", ""), row.get("param_value", "")))
    except Exception as exc:
        errors.append(str(exc))

    _safe_audit(request, action="strategy_save", resource_type="strategy_config",
                resource_id="", result="failed" if errors else "success",
                details="; ".join(errors) if errors else "", user=user)
    if errors:
        return _json_error("; ".join(errors), 400)
    return JSONResponse({"ok": True, "message": "策略配置已保存，将在 30 秒内自动生效，无需重启。"})


@app.get("/api/strategy/runtime")
async def api_strategy_runtime(request: Request):
    _session_user(request, "viewer")
    _require_db()
    rows = _db.fetch_all("""
        SELECT rs.strategy_id, sd.name, sd.type, sd.enabled, sd.device_id,
               rs.last_execution_time, rs.current_target_value,
               rs.current_target_point_id, rs.suppressed, rs.suppress_reason,
               rs.manual_override_until, rs.last_error, rs.input_summary,
               rs.updated_at
        FROM strategy_runtime_state rs
        JOIN strategy_definitions sd ON sd.id = rs.strategy_id
        ORDER BY sd.priority ASC
    """)
    for row in rows:
        row["enabled"] = bool(row.get("enabled"))
        row["suppressed"] = bool(row.get("suppressed"))
        if row.get("last_execution_time"):
            row["last_execution_time"] = str(row["last_execution_time"])
        if row.get("manual_override_until"):
            row["manual_override_until"] = str(row["manual_override_until"])
        if row.get("updated_at"):
            row["updated_at"] = str(row["updated_at"])
    return JSONResponse({"rows": rows, "count": len(rows)})


@app.get("/api/strategy/logs")
async def api_strategy_logs(request: Request, limit: int = 200):
    _session_user(request, "viewer")
    _require_db()
    rows = _db.fetch_all("""
        SELECT id, strategy_id, action_type, target_point_id, desired_value,
               result_value, suppress_reason, input_summary, result, details,
               created_at
        FROM strategy_action_logs
        ORDER BY created_at DESC
        LIMIT %s
    """, (min(limit, 1000),))
    for row in rows:
        if row.get("created_at"):
            row["created_at"] = str(row["created_at"])
    return JSONResponse({"rows": rows, "count": len(rows)})


# ---- System Monitoring ----


def _run_cmd(args: List[str], timeout: int = 5) -> str:
    try:
        return subprocess.check_output(args, timeout=timeout, stderr=subprocess.STDOUT).decode("utf-8", errors="replace")
    except Exception as e:
        return f"(error: {e})"


def _service_pid_path(service: str) -> Path:
    return PID_DIR / f"{service}.pid"


def _service_log_path(service: str) -> Path:
    return LOG_DIR / f"{service}.log"


def _service_note(service: str) -> str:
    spec = SERVICE_SPECS.get(service, {})
    return str(spec.get("note", ""))


def _identify_service(comm: str, cmdline: str) -> Optional[str]:
    command = (cmdline or "").lower()
    name = (comm or "").lower()
    if "run_dashboard.py" in command or "uvicorn" in command:
        return "web"
    if "openems-rtdb-service" in command or name.startswith("openems-rtdb"):
        return "rtdb"
    if "openems-modbus-collector" in command or name.startswith("openems-modbus"):
        return "modbus"
    if "openems-iec104-collector" in command or name.startswith("openems-iec104"):
        return "iec104"
    if "openems-history" in command or name.startswith("openems-history"):
        return "history"
    if "openems-alarm" in command or name.startswith("openems-alarm"):
        return "alarm"
    if "openems-strategy-engine" in command or name.startswith("openems-strateg"):
        return "strategy"
    return None


def _service_display_name(service: Optional[str], comm: str, cmdline: str) -> str:
    if service and service in SERVICE_SPECS:
        return str(SERVICE_SPECS[service]["display_name"])
    if cmdline:
        first = cmdline.split()[0]
        return Path(first).name or comm
    return comm


def _read_pidfile(service: str) -> Optional[int]:
    path = _service_pid_path(service)
    try:
        return int(path.read_text(encoding="utf-8").strip())
    except Exception:
        return None


def _pid_exists(pid: Optional[int]) -> bool:
    if pid is None or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except Exception:
        return False


def _remove_pidfile(service: str) -> None:
    try:
        _service_pid_path(service).unlink(missing_ok=True)
    except Exception:
        pass


def _service_status(service: str) -> str:
    pid = _read_pidfile(service)
    if pid is not None and _pid_exists(pid):
        return "running"
    return "stopped"


def _service_control_available() -> bool:
    return os.name != "nt" and APP_ROOT.as_posix().startswith("/opt/openems")


def _start_managed_service(service: str) -> Dict[str, Any]:
    if service not in SERVICE_SPECS:
        raise ValueError("Unknown service")
    spec = SERVICE_SPECS[service]
    if not spec.get("controllable", False):
        raise RuntimeError(_service_note(service) or "Service control is not supported for this process.")
    if _service_status(service) == "running":
        logger.info("System service start skipped: %s already running", service)
        return {"service": service, "status": "running", "message": "Service is already running."}

    PID_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = _service_log_path(service)
    with open(log_path, "a", encoding="utf-8", errors="replace") as log_handle:
        proc = subprocess.Popen(
            spec["command"],
            cwd=str(APP_ROOT),
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            env=os.environ.copy(),
        )
    _service_pid_path(service).write_text(str(proc.pid), encoding="utf-8")
    logger.info("System service started: %s pid=%s command=%s", service, proc.pid, " ".join(spec["command"]))
    return {"service": service, "status": "running", "pid": proc.pid}


def _stop_managed_service(service: str) -> Dict[str, Any]:
    if service not in SERVICE_SPECS:
        raise ValueError("Unknown service")
    spec = SERVICE_SPECS[service]
    if not spec.get("controllable", False):
        raise RuntimeError(_service_note(service) or "Service control is not supported for this process.")
    pid = _read_pidfile(service)
    if pid is None or not _pid_exists(pid):
        _remove_pidfile(service)
        logger.info("System service stop skipped: %s already stopped", service)
        return {"service": service, "status": "stopped", "message": "Service is not running."}

    logger.info("System service stopping: %s pid=%s", service, pid)
    os.kill(pid, signal.SIGTERM)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if not _pid_exists(pid):
            _remove_pidfile(service)
            logger.info("System service stopped: %s pid=%s", service, pid)
            return {"service": service, "status": "stopped", "pid": pid}
        time.sleep(0.2)
    os.kill(pid, signal.SIGKILL)
    _remove_pidfile(service)
    logger.warning("System service force-killed: %s pid=%s", service, pid)
    return {"service": service, "status": "stopped", "pid": pid, "forced": True}


def _restart_managed_service(service: str) -> Dict[str, Any]:
    logger.info("System service restarting: %s", service)
    stop_result = _stop_managed_service(service)
    start_result = _start_managed_service(service)
    return {
        "service": service,
        "status": start_result.get("status", "running"),
        "previous": stop_result,
        "current": start_result,
    }


def _is_openems_process_name(label: str) -> bool:
    value = (label or "").lower()
    return any(
        token in value
        for token in ("openems", "uvicorn", "python")
    )


def _is_openems_process_identity(label: str) -> bool:
    value = (label or "").lower()
    return (
        "/openems-" in value
        or "run_dashboard.py" in value
        or "uvicorn" in value
        or "python" in value
    )


def _parse_linux_process_list_from_proc() -> List[Dict[str, Any]]:
    procs: List[Dict[str, Any]] = []
    proc_root = Path("/proc")
    if not proc_root.exists():
        return procs

    try:
        mem_total_kb = 0.0
        with open("/proc/meminfo", "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if line.startswith("MemTotal:"):
                    mem_total_kb = float(line.split()[1])
                    break
        with open("/proc/uptime", "r", encoding="utf-8", errors="replace") as handle:
            uptime_seconds = float(handle.read().split()[0])
        clock_ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        page_size = os.sysconf("SC_PAGE_SIZE")
        cpu_count = max(os.cpu_count() or 1, 1)
    except Exception:
        return procs

    for entry in proc_root.iterdir():
        if not entry.is_dir() or not entry.name.isdigit():
            continue
        try:
            pid = int(entry.name)
            comm = (entry / "comm").read_text(encoding="utf-8", errors="replace").strip()
            cmdline = (entry / "cmdline").read_text(encoding="utf-8", errors="replace").replace("\x00", " ").strip()
            identity = cmdline or comm
            service = _identify_service(comm, cmdline)
            if not (service or _is_openems_process_name(comm) or _is_openems_process_identity(identity)):
                continue

            stat_parts = (entry / "stat").read_text(encoding="utf-8", errors="replace").split()
            if len(stat_parts) < 24:
                continue
            ppid = int(stat_parts[3])
            utime = float(stat_parts[13])
            stime = float(stat_parts[14])
            rss_pages = float(stat_parts[23])

            rss_bytes = rss_pages * page_size
            rss_kb = rss_bytes / 1024.0
            mem_mb = rss_bytes / (1024.0 * 1024.0)
            mem_percent = (rss_kb / mem_total_kb * 100.0) if mem_total_kb > 0 else 0.0
            cpu_seconds = (utime + stime) / float(clock_ticks)
            cpu_percent = (
                cpu_seconds / max(uptime_seconds, 1.0) * 100.0 / float(cpu_count)
            )

            procs.append(
                {
                    "pid": pid,
                    "ppid": ppid,
                    "cpu_percent": round(cpu_percent, 1),
                    "mem_percent": round(mem_percent, 1),
                    "rss_mb": round(mem_mb, 1),
                    "name": _service_display_name(service, comm, cmdline),
                    "short_name": comm,
                    "service": service or "",
                    "cmdline": cmdline,
                    "status": "running",
                }
            )
        except Exception:
            continue
    return procs


def _parse_windows_process_list() -> List[Dict[str, Any]]:
    procs: List[Dict[str, Any]] = []
    try:
        out = subprocess.check_output(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "Get-Process | Select-Object Id,ProcessName,CPU,WS | ConvertTo-Json -Compress",
            ],
            timeout=5,
            stderr=subprocess.STDOUT,
        ).decode("utf-8", errors="replace")
    except Exception:
        try:
            out = subprocess.check_output(
                ["tasklist", "/FO", "CSV", "/NH"],
                timeout=5,
                stderr=subprocess.STDOUT,
            ).decode("utf-8", errors="replace")
            for row in csv.reader(out.splitlines()):
                if len(row) < 5:
                    continue
                image_name, pid_text, _session_name, _session_num, mem_usage = row[:5]
                base_name = Path(image_name).stem
                if not _is_openems_process_name(base_name):
                    continue
                try:
                    pid = int(pid_text)
                except ValueError:
                    continue
                mem_kb = float(mem_usage.replace(",", "").replace(" K", "").replace(" KB", "").strip() or "0")
                service = _identify_service(base_name, image_name)
                procs.append(
                    {
                        "pid": pid,
                        "ppid": 0,
                        "cpu_percent": 0.0,
                        "mem_percent": 0.0,
                        "rss_mb": round(mem_kb / 1024.0, 1),
                        "name": _service_display_name(service, base_name, image_name),
                        "short_name": base_name,
                        "service": service or "",
                        "cmdline": image_name,
                        "status": "running",
                    }
                )
        except Exception:
            return procs
        return procs

    try:
        import json

        rows = json.loads(out)
        if isinstance(rows, dict):
            rows = [rows]
        for row in rows or []:
            name = str(row.get("ProcessName") or "")
            if not _is_openems_process_name(name):
                continue
            rss_bytes = float(row.get("WS") or 0.0)
            service = _identify_service(name, name)
            procs.append(
                {
                    "pid": int(row.get("Id") or 0),
                    "ppid": 0,
                    "cpu_percent": 0.0,
                    "mem_percent": 0.0,
                    "rss_mb": round(rss_bytes / (1024.0 * 1024.0), 1),
                    "name": _service_display_name(service, name, name),
                    "short_name": name,
                    "service": service or "",
                    "cmdline": name,
                    "status": "running",
                }
            )
    except Exception:
        return []
    return procs


def _parse_process_list() -> List[Dict[str, Any]]:
    """Return OpenEMS-related processes across Windows and Linux containers."""
    if sys.platform.startswith("win"):
        return _parse_windows_process_list()

    if Path("/proc").exists():
        proc_procs = _parse_linux_process_list_from_proc()
        if proc_procs:
            return proc_procs

    if shutil.which("ps") is None:
        return []

    procs: List[Dict[str, Any]] = []
    try:
        out = subprocess.check_output(
            ["ps", "-eo", "pid,ppid,%cpu,%mem,rss,comm", "--no-headers"],
            timeout=5, stderr=subprocess.STDOUT
        ).decode("utf-8", errors="replace")
        for line in out.strip().split("\n"):
            parts = line.split(None, 5)
            if len(parts) >= 6:
                pid = parts[0]
                ppid = parts[1]
                cpu = parts[2]
                mem = parts[3]
                rss_kb = parts[4]
                comm = parts[5]
                if _is_openems_process_name(comm):
                    mem_mb = float(rss_kb) / 1024.0 if rss_kb else 0.0
                    service = _identify_service(comm, comm)
                    procs.append({
                        "pid": int(pid),
                        "ppid": int(ppid),
                        "cpu_percent": float(cpu),
                        "mem_percent": float(mem),
                        "rss_mb": round(mem_mb, 1),
                        "name": _service_display_name(service, comm, comm),
                        "short_name": comm,
                        "service": service or "",
                        "cmdline": comm,
                        "status": "running",
                    })
    except Exception:
        pass
    return procs


def _read_linux_cpu_times() -> Optional[Dict[str, float]]:
    try:
        with open("/proc/stat", "r", encoding="utf-8", errors="replace") as handle:
            first = handle.readline().split()
        if len(first) < 8 or first[0] != "cpu":
            return None
        values = [float(item) for item in first[1:]]
        idle = values[3] + (values[4] if len(values) > 4 else 0.0)
        total = sum(values)
        return {"idle": idle, "total": total}
    except Exception:
        return None


def _read_linux_network_counters() -> Optional[Dict[str, Any]]:
    try:
        rx_total = 0
        tx_total = 0
        interfaces: List[str] = []
        with open("/proc/net/dev", "r", encoding="utf-8", errors="replace") as handle:
            for line in handle.readlines()[2:]:
                if ":" not in line:
                    continue
                name, payload = line.split(":", 1)
                iface = name.strip()
                if not iface or iface == "lo":
                    continue
                parts = payload.split()
                if len(parts) < 16:
                    continue
                rx_total += int(parts[0])
                tx_total += int(parts[8])
                interfaces.append(iface)
        return {"rx_bytes": rx_total, "tx_bytes": tx_total, "interfaces": interfaces}
    except Exception:
        return None


def _system_resources() -> Dict[str, Any]:
    """Read /proc for system-level resource info."""
    info: Dict[str, Any] = {
        "cpu_percent": 0.0,
        "net_rx_bytes": 0,
        "net_tx_bytes": 0,
        "net_rx_bps": 0.0,
        "net_tx_bps": 0.0,
        "net_interfaces": [],
        "net_interface_count": 0,
    }
    now = time.time()

    # Memory
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    info["mem_total_kb"] = int(line.split()[1])
                elif line.startswith("MemAvailable:"):
                    info["mem_available_kb"] = int(line.split()[1])
    except Exception:
        info["mem_total_kb"] = 0
        info["mem_available_kb"] = 0

    if info.get("mem_total_kb", 0) > 0:
        info["mem_used_percent"] = round(
            (1 - info["mem_available_kb"] / info["mem_total_kb"]) * 100, 1
        )
        info["mem_total_mb"] = round(info["mem_total_kb"] / 1024.0, 1)
        info["mem_used_mb"] = round(
            (info["mem_total_kb"] - info["mem_available_kb"]) / 1024.0, 1
        )

    cpu_times = _read_linux_cpu_times()
    if cpu_times:
        prev_total = _system_metrics_cache.get("cpu_total")
        prev_idle = _system_metrics_cache.get("cpu_idle")
        if prev_total is not None and prev_idle is not None:
            total_delta = cpu_times["total"] - float(prev_total)
            idle_delta = cpu_times["idle"] - float(prev_idle)
            if total_delta > 0:
                info["cpu_percent"] = round((1.0 - idle_delta / total_delta) * 100.0, 1)
        _system_metrics_cache["cpu_total"] = cpu_times["total"]
        _system_metrics_cache["cpu_idle"] = cpu_times["idle"]

    # Load average
    try:
        with open("/proc/loadavg") as f:
            parts = f.read().strip().split()
            info["load_1m"] = float(parts[0])
            info["load_5m"] = float(parts[1])
            info["load_15m"] = float(parts[2])
    except Exception:
        pass

    # Uptime
    try:
        with open("/proc/uptime") as f:
            info["uptime_seconds"] = float(f.read().split()[0])
    except Exception:
        pass

    net = _read_linux_network_counters()
    previous_ts = _system_metrics_cache.get("ts")
    if net:
        info["net_rx_bytes"] = net["rx_bytes"]
        info["net_tx_bytes"] = net["tx_bytes"]
        info["net_interfaces"] = net["interfaces"]
        info["net_interface_count"] = len(net["interfaces"])
        prev_rx = _system_metrics_cache.get("net_rx_bytes")
        prev_tx = _system_metrics_cache.get("net_tx_bytes")
        if (
            previous_ts is not None
            and prev_rx is not None
            and prev_tx is not None
            and now > float(previous_ts)
        ):
            dt = now - float(previous_ts)
            info["net_rx_bps"] = max(0.0, (net["rx_bytes"] - float(prev_rx)) / dt)
            info["net_tx_bps"] = max(0.0, (net["tx_bytes"] - float(prev_tx)) / dt)
        _system_metrics_cache["net_rx_bytes"] = net["rx_bytes"]
        _system_metrics_cache["net_tx_bytes"] = net["tx_bytes"]

    # Disk
    try:
        disk_target = "/opt/openems" if Path("/opt/openems").exists() else str(APP_ROOT)
        out = subprocess.check_output(
            ["df", "-h", disk_target], timeout=5, stderr=subprocess.STDOUT
        ).decode("utf-8", errors="replace")
        lines = out.strip().split("\n")
        if len(lines) >= 2:
            parts = lines[1].split()
            if len(parts) >= 6:
                info["disk_total"] = parts[1]
                info["disk_used"] = parts[2]
                info["disk_available"] = parts[3]
                info["disk_use_percent"] = parts[4]
    except Exception:
        pass

    _system_metrics_cache["ts"] = now
    info["sample_time"] = datetime.now(timezone.utc).isoformat()
    return info


def _managed_service_rows() -> List[Dict[str, Any]]:
    running = _parse_process_list()
    by_service: Dict[str, Dict[str, Any]] = {}
    for proc in running:
        service = str(proc.get("service") or "")
        if service:
            by_service[service] = proc

    rows: List[Dict[str, Any]] = []
    for service, spec in SERVICE_SPECS.items():
        proc = by_service.get(service)
        pid = _read_pidfile(service)
        if proc:
            rows.append(
                {
                    "service": service,
                    "display_name": proc.get("name") or spec["display_name"],
                    "name": proc.get("name") or spec["display_name"],
                    "short_name": proc.get("short_name") or spec["display_name"],
                    "pid": proc.get("pid"),
                    "ppid": proc.get("ppid", 0),
                    "cpu_percent": float(proc.get("cpu_percent") or 0.0),
                    "mem_percent": float(proc.get("mem_percent") or 0.0),
                    "rss_mb": float(proc.get("rss_mb") or 0.0),
                    "cmdline": proc.get("cmdline") or " ".join(spec["command"]),
                    "status": "running",
                    "controllable": bool(spec.get("controllable", False) and _service_control_available()),
                    "note": _service_note(service),
                }
            )
        else:
            rows.append(
                {
                    "service": service,
                    "display_name": spec["display_name"],
                    "name": spec["display_name"],
                    "short_name": spec["display_name"],
                    "pid": pid or 0,
                    "ppid": 0,
                    "cpu_percent": 0.0,
                    "mem_percent": 0.0,
                    "rss_mb": 0.0,
                    "cmdline": " ".join(spec["command"]),
                    "status": _service_status(service),
                    "controllable": bool(spec.get("controllable", False) and _service_control_available()),
                    "note": _service_note(service),
                }
            )
    return rows


@app.get("/api/system/processes")
async def api_system_processes(request: Request):
    _session_user(request, "viewer")
    processes = _managed_service_rows()
    return JSONResponse({
        "processes": processes,
        "count": len(processes),
    })


@app.get("/api/system/resources")
async def api_system_resources(request: Request):
    _session_user(request, "viewer")
    return JSONResponse(_system_resources())


@app.get("/api/system/logs")
async def api_system_logs(request: Request, service: str = "", lines: int = 200):
    _session_user(request, "viewer")
    log_dir = Path(os.environ.get("APP_ROOT", "/opt/openems/install")) / "runtime" / "logs"
    if not log_dir.exists():
        return JSONResponse({"error": "Log directory not found"}, status_code=404)

    if service:
        log_path = log_dir / f"{service}.log"
        if not log_path.exists():
            return JSONResponse({"error": f"Log file not found: {service}.log"}, status_code=404)
        try:
            # Read last N lines efficiently
            with open(log_path, "rb") as f:
                f.seek(0, 2)
                size = f.tell()
                buf = b""
                chunk_size = 4096
                while len(buf.split(b"\n")) <= lines and size > 0:
                    read_size = min(chunk_size, size)
                    size -= read_size
                    f.seek(size)
                    buf = f.read(read_size) + buf
                log_lines = buf.decode("utf-8", errors="replace").split("\n")
                return JSONResponse({
                    "service": service,
                    "lines": log_lines[-lines:] if len(log_lines) > lines else log_lines,
                })
        except Exception as e:
            return JSONResponse({"error": str(e)}, status_code=500)

    # List available services
    services = []
    for f in sorted(log_dir.glob("*.log")):
        svc_name = f.stem
        try:
            fsize = f.stat().st_size
            services.append({"name": svc_name, "size_bytes": fsize})
        except Exception:
            services.append({"name": svc_name, "size_bytes": 0})
    return JSONResponse({"services": services})


@app.post("/api/system/services/{service}/{action}")
async def api_system_service_action(request: Request, service: str, action: str):
    user = _session_user(request, "admin")
    service = service.strip().lower()
    action = action.strip().lower()
    if service not in SERVICE_SPECS:
        raise HTTPException(status_code=404, detail="Unknown service.")
    if action not in {"start", "stop", "restart"}:
        raise HTTPException(status_code=400, detail="Unsupported action.")
    if not _service_control_available():
        raise HTTPException(status_code=400, detail="Service control is only supported in the Linux install/container runtime.")
    logger.info("System service action requested: service=%s action=%s user=%s", service, action, user.get("username"))
    try:
        if action == "start":
            result = _start_managed_service(service)
        elif action == "stop":
            result = _stop_managed_service(service)
        else:
            result = _restart_managed_service(service)
    except RuntimeError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    except Exception as exc:
        logger.exception("System service action failed: service=%s action=%s", service, action)
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    try:
        _safe_audit(
            request,
            action=f"system_service_{action}",
            resource_type="system_service",
            resource_id=service,
            result="success",
            after_json=result,
            user=user,
        )
    except Exception:
        logger.exception("Failed to write audit log for system service action: %s %s", service, action)
    logger.info("System service action completed: service=%s action=%s result=%s", service, action, result)
    return JSONResponse({"ok": True, "result": result})


@app.get("/system", response_class=HTMLResponse)
async def system_page(request: Request):
    return _page_response(request, "system_admin.html")


@app.get("/api/audit")
async def api_audit(request: Request, action: str = "", username: str = "", limit: int = 200):
    _session_user(request, "admin")
    _require_db()
    rows = _db.list_audit_logs(action=action, username=username, limit=limit)
    return JSONResponse({"rows": rows, "count": len(rows)})
