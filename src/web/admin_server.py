"""OpenEMS single-site operations console with PostgreSQL-backed admin features."""

from __future__ import annotations

import asyncio
import csv
import json
import time
from datetime import datetime, timedelta, timezone
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
from shm_reader import ShmReader

WEB_DIR = Path(__file__).parent
CONFIG_DIR = Path(__file__).parent.parent.parent / "config" / "tables"
ALARM_FILE = Path("runtime") / "alarms_active.json"
HISTORY_DIR = Path("runtime") / "history"
MIGRATIONS_DIR = WEB_DIR / "migrations"

ROLE_LEVELS = {"viewer": 1, "operator": 2, "admin": 3}

app = FastAPI(title="OpenEMS Operations Console")
app.mount("/assets", StaticFiles(directory=str(WEB_DIR / "assets")), name="assets")

_reader = ShmReader()
_config_store = ConfigStore(CONFIG_DIR, backup_root=Path("runtime") / "config_backups")
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


def _active_alarm_file_payload() -> Dict[str, Any]:
    if not ALARM_FILE.exists():
        return {"generated_at": 0, "count": 0, "alarms": []}
    try:
        with ALARM_FILE.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        alarms = data.get("alarms", [])
        return {"generated_at": data.get("generated_at", 0), "count": len(alarms), "alarms": alarms}
    except Exception as exc:
        return {"generated_at": 0, "count": 0, "alarms": [], "error": "Failed to read active alarm file: " + str(exc)}


def _point_lookup() -> Dict[str, Dict[str, Any]]:
    lookup: Dict[str, Dict[str, Any]] = {}
    for category, filename in POINT_TABLES.items():
        path = CONFIG_DIR / filename
        if not path.exists():
            continue
        with path.open(encoding="utf-8") as handle:
            reader = csv.DictReader(line for line in handle if line.strip() and not line.lstrip().startswith("#"))
            for row in reader:
                point_id = (row.get("id") or "").strip()
                if not point_id:
                    continue
                lookup[point_id] = {
                    "id": point_id,
                    "device_id": (row.get("device_id") or "").strip(),
                    "name": (row.get("name") or point_id).strip(),
                    "category": category,
                    "unit": (row.get("unit") or "").strip(),
                }
    return lookup


def _sync_active_alarms_to_db() -> Dict[str, Any]:
    payload = _active_alarm_file_payload()
    if _db_ready():
        point_lookup = _point_lookup()
        alarms = []
        for alarm in payload.get("alarms", []):
            enriched = dict(alarm)
            point_meta = point_lookup.get((alarm.get("point_id") or "").strip(), {})
            if not enriched.get("device_id"):
                enriched["device_id"] = point_meta.get("device_id", "")
            if not enriched.get("value_display"):
                value = enriched.get("value")
                unit = point_meta.get("unit") or enriched.get("unit") or ""
                enriched["value_display"] = f"{value} {unit}".strip() if value is not None else ""
            alarms.append(enriched)
        _db.sync_active_alarms(alarms)
    return payload


def _read_point_metadata() -> List[Dict[str, Any]]:
    return list(_point_lookup().values())


def _history_day_files(start_ms: int, end_ms: int) -> List[Path]:
    start_day = datetime.fromtimestamp(start_ms / 1000).date()
    end_day = datetime.fromtimestamp(end_ms / 1000).date()
    paths: List[Path] = []
    current = start_day
    while current <= end_day:
        paths.append(HISTORY_DIR / f"{current:%Y%m%d}.jsonl")
        current = current + timedelta(days=1)
    return paths


def _config_overview() -> Dict[str, Any]:
    data = _config_store.load()
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


@app.on_event("startup")
async def startup():
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
    alarm_payload = _sync_active_alarms_to_db()
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
    return JSONResponse(_sync_active_alarms_to_db())


@app.get("/api/alarms")
async def api_alarms(request: Request, status: str = "active", severity: str = "", device_id: str = "", limit: int = 200):
    _session_user(request, "viewer")
    _sync_active_alarms_to_db()
    _require_db()
    rows = _db.list_alarms(status=status or None, severity=severity or None, device_id=device_id or None, limit=limit)
    return JSONResponse({"rows": rows, "count": len(rows)})


@app.post("/api/alarms/{alarm_id}/ack")
async def api_alarm_ack(request: Request, alarm_id: str):
    user = _session_user(request, "operator")
    _sync_active_alarms_to_db()
    row = _db.ack_alarm(alarm_id, str(user["username"]))
    if not row:
        return _json_error("Alarm not found.", 404)
    _safe_audit(request, action="alarm_ack", resource_type="alarm", resource_id=alarm_id, result="success", after_json=row, user=user)
    return JSONResponse({"ok": True, "alarm": row})


@app.post("/api/alarms/{alarm_id}/silence")
async def api_alarm_silence(request: Request, alarm_id: str):
    user = _session_user(request, "operator")
    _sync_active_alarms_to_db()
    row = _db.silence_alarm(alarm_id, str(user["username"]))
    if not row:
        return _json_error("Alarm not found.", 404)
    _safe_audit(request, action="alarm_silence", resource_type="alarm", resource_id=alarm_id, result="success", after_json=row, user=user)
    return JSONResponse({"ok": True, "alarm": row})


@app.get("/api/history/points")
async def api_history_points(request: Request):
    _session_user(request, "viewer")
    return JSONResponse({"points": _read_point_metadata()})


@app.get("/api/history/query")
async def api_history_query(request: Request, point_id: str, start: Optional[int] = None, end: Optional[int] = None, limit: int = 5000):
    _session_user(request, "viewer")
    now = int(time.time() * 1000)
    end_ms = end if end is not None else now
    start_ms = start if start is not None else end_ms - 3600 * 1000
    if start_ms > end_ms:
        start_ms, end_ms = end_ms, start_ms

    limit = max(1, min(limit, 20000))
    rows: List[Dict[str, Any]] = []
    for path in _history_day_files(start_ms, end_ms):
        if not path.exists():
            continue
        try:
            with path.open("r", encoding="utf-8") as handle:
                for line in handle:
                    if len(rows) >= limit:
                        break
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if record.get("point_id") != point_id:
                        continue
                    ts = int(record.get("ts") or 0)
                    if ts < start_ms or ts > end_ms:
                        continue
                    rows.append({"ts": ts, "value": record.get("value"), "quality": record.get("quality", "Unknown"), "valid": bool(record.get("valid", False))})
        except OSError:
            continue
        if len(rows) >= limit:
            break
    return JSONResponse({"point_id": point_id, "count": len(rows), "rows": rows})


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


@app.get("/api/comm/schema")
async def api_comm_schema(request: Request):
    _session_user(request, "viewer")
    return JSONResponse(_config_store.schema())


@app.get("/api/comm/data")
async def api_comm_data(request: Request):
    _session_user(request, "viewer")
    return JSONResponse({"tables": await asyncio.to_thread(_config_store.load)})


@app.post("/api/comm/validate")
async def api_comm_validate(request: Request, req: ConfigEditorRequest):
    _session_user(request, "admin")
    result = await asyncio.to_thread(_config_store.validate, _model_dump(req))
    status_code = 200 if result["ok"] else 400
    return JSONResponse(result, status_code=status_code)


@app.post("/api/comm/save")
async def api_comm_save(request: Request, req: ConfigEditorRequest):
    user = _session_user(request, "admin")
    before_tables = await asyncio.to_thread(_config_store.load)
    result = await asyncio.to_thread(_config_store.save, _model_dump(req))
    status_code = 200 if result["ok"] else 400
    _safe_audit(
        request,
        action="comm_save",
        resource_type="config_tables",
        resource_id="config/tables",
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


@app.get("/api/audit")
async def api_audit(request: Request, action: str = "", username: str = "", limit: int = 200):
    _session_user(request, "admin")
    _require_db()
    rows = _db.list_audit_logs(action=action, username=username, limit=limit)
    return JSONResponse({"rows": rows, "count": len(rows)})
