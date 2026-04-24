"""OpenEMS single-site operations console with PostgreSQL-backed admin features."""

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


class CommandRequest(BaseModel):
    point_id: str
    desired_value: float


class ConfigEditorRequest(BaseModel):
    tables: Dict[str, List[Dict[str, Any]]]


POINT_TABLES = {
    "telemetry": "telemetry.csv",
    "teleindication": "teleindication.csv",
    "telecontrol": "telecontrol.csv",
    "teleadjust": "teleadjust.csv",
}


def _read_point_metadata() -> List[Dict[str, Any]]:
    points: List[Dict[str, Any]] = []
    for category, filename in POINT_TABLES.items():
        path = CONFIG_DIR / filename
        try:
            with path.open(encoding="utf-8") as handle:
                reader = csv.DictReader(handle)
                for row in reader:
                    if not any((value or "").strip() for value in row.values()):
                        continue
                    point_id = (row.get("id") or "").strip()
                    if not point_id:
                        continue
                    points.append({
                        "id": point_id,
                        "device_id": (row.get("device_id") or "").strip(),
                        "name": (row.get("name") or point_id).strip(),
                        "category": category,
                        "unit": (row.get("unit") or "").strip(),
                    })
        except FileNotFoundError:
            continue
    return points


def _history_day_files(start_ms: int, end_ms: int) -> List[Path]:
    start_day = datetime.fromtimestamp(start_ms / 1000).date()
    end_day = datetime.fromtimestamp(end_ms / 1000).date()
    paths: List[Path] = []
    current = start_day
    while current <= end_day:
        paths.append(HISTORY_DIR / f"{current:%Y%m%d}.jsonl")
        current = current + timedelta(days=1)
    return paths


@app.on_event("startup")
async def startup():
    _reader.attach()


@app.get("/", response_class=HTMLResponse)
async def dashboard():
    html_path = WEB_DIR / "dashboard.html"
    return HTMLResponse(html_path.read_text(encoding="utf-8"))


@app.get("/config", response_class=HTMLResponse)
async def config_console():
    html_path = WEB_DIR / "config.html"
    return HTMLResponse(html_path.read_text(encoding="utf-8"))


@app.get("/history", response_class=HTMLResponse)
async def history_page():
    html_path = WEB_DIR / "history.html"
    return HTMLResponse(html_path.read_text(encoding="utf-8"))


@app.get("/api/snapshot")
async def api_snapshot():
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
async def api_submit_command(req: CommandRequest):
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if not _reader.is_attached():
        return JSONResponse({"error": "Shared memory not available"}, status_code=503)
    result = await asyncio.to_thread(_reader.submit_command, req.point_id, req.desired_value)
    if "error" in result:
        return JSONResponse(result, status_code=400)
    return JSONResponse(result)


@app.get("/api/command/{point_id}")
async def api_command_status(point_id: str):
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if not _reader.is_attached():
        return JSONResponse({"error": "Shared memory not available"}, status_code=503)
    result = await asyncio.to_thread(_reader.read_command_status, point_id)
    if "error" in result:
        return JSONResponse(result, status_code=404)
    return JSONResponse(result)


@app.get("/api/alarms/active")
async def api_active_alarms():
    if not ALARM_FILE.exists():
        return JSONResponse({"generated_at": 0, "count": 0, "alarms": []})

    try:
        with ALARM_FILE.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        alarms = data.get("alarms", [])
        return JSONResponse({
            "generated_at": data.get("generated_at", 0),
            "count": len(alarms),
            "alarms": alarms,
        })
    except Exception as exc:
        return JSONResponse({
            "generated_at": 0,
            "count": 0,
            "alarms": [],
            "error": "Failed to read active alarm file: " + str(exc),
        }, status_code=503)


@app.get("/api/history/points")
async def api_history_points():
    return JSONResponse({"points": _read_point_metadata()})


@app.get("/api/history/query")
async def api_history_query(
    point_id: str,
    start: Optional[int] = None,
    end: Optional[int] = None,
    limit: int = 5000,
):
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
                    rows.append({
                        "ts": ts,
                        "value": record.get("value"),
                        "quality": record.get("quality", "Unknown"),
                        "valid": bool(record.get("valid", False)),
                    })
        except OSError:
            continue
        if len(rows) >= limit:
            break

    return JSONResponse({
        "point_id": point_id,
        "count": len(rows),
        "rows": rows,
    })


@app.get("/api/config")
async def api_config():
    devices = []
    try:
        with open(CONFIG_DIR / "device.csv", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                if any((value or "").strip() for value in row.values()):
                    devices.append(row)
    except Exception:
        pass

    telemetry = []
    try:
        with open(CONFIG_DIR / "telemetry.csv", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                if any((value or "").strip() for value in row.values()):
                    telemetry.append(row)
    except Exception:
        pass

    teleindication = []
    try:
        with open(CONFIG_DIR / "teleindication.csv", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                if any((value or "").strip() for value in row.values()):
                    teleindication.append(row)
    except Exception:
        pass

    telecontrol = []
    try:
        with open(CONFIG_DIR / "telecontrol.csv", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                if any((value or "").strip() for value in row.values()):
                    telecontrol.append(row)
    except Exception:
        pass

    teleadjust = []
    try:
        with open(CONFIG_DIR / "teleadjust.csv", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                if any((value or "").strip() for value in row.values()):
                    teleadjust.append(row)
    except Exception:
        pass

    return JSONResponse({
        "devices": devices,
        "telemetry": telemetry,
        "teleindication": teleindication,
        "telecontrol": telecontrol,
        "teleadjust": teleadjust,
    })


@app.get("/api/config-editor/schema")
async def api_config_editor_schema():
    return JSONResponse(_config_store.schema())


@app.get("/api/config-editor/data")
async def api_config_editor_data():
    return JSONResponse({"tables": await asyncio.to_thread(_config_store.load)})


@app.post("/api/config-editor/validate")
async def api_config_editor_validate(req: ConfigEditorRequest):
    result = await asyncio.to_thread(_config_store.validate, req.model_dump())
    status_code = 200 if result["ok"] else 400
    return JSONResponse(result, status_code=status_code)


@app.post("/api/config-editor/save")
async def api_config_editor_save(req: ConfigEditorRequest):
    result = await asyncio.to_thread(_config_store.save, req.model_dump())
    status_code = 200 if result["ok"] else 400
    return JSONResponse(result, status_code=status_code)


from admin_server import app  # noqa: E402,F401
