"""OpenEMS single-site operations console with PostgreSQL-backed admin features."""

from __future__ import annotations

import asyncio
import logging
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
            "SELECT id FROM strategy_definitions WHERE device_id = %s AND enabled = TRUE",
            (device_id,),
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


@app.get("/api/audit")
async def api_audit(request: Request, action: str = "", username: str = "", limit: int = 200):
    _session_user(request, "admin")
    _require_db()
    rows = _db.list_audit_logs(action=action, username=username, limit=limit)
    return JSONResponse({"rows": rows, "count": len(rows)})
