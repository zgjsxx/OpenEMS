"""OpenEMS Web Dashboard — FastAPI server.

Serves a real-time dashboard that reads data from the OpenEMS RtDb
shared memory and displays it as a web page.
Also supports command submission for telecontrol/setting write operations.
"""

import os
import csv
import asyncio
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import HTMLResponse, JSONResponse
from pydantic import BaseModel

from shm_reader import ShmReader

WEB_DIR = Path(__file__).parent
CONFIG_DIR = Path(__file__).parent.parent.parent / "config" / "tables"

app = FastAPI(title="OpenEMS Dashboard")

# Global reader — attached once on startup
_reader = ShmReader()


class CommandRequest(BaseModel):
    point_id: str
    desired_value: float


@app.on_event("startup")
async def startup():
    _reader.attach()


@app.get("/", response_class=HTMLResponse)
async def dashboard():
    html_path = WEB_DIR / "dashboard.html"
    return HTMLResponse(html_path.read_text(encoding="utf-8"))


@app.get("/api/snapshot")
async def api_snapshot():
    # Auto-reconnect if not attached (e.g. collector wasn't running at startup)
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if not _reader.is_attached():
        return JSONResponse({"error": "Shared memory not available. Is openems-modbus-collector running?"}, status_code=503)
    # Run blocking ctypes read in thread pool to avoid blocking uvicorn
    data = await asyncio.to_thread(_reader.read_snapshot)
    # If read failed, detach and retry next time
    if "error" in data and data.get("points", []) == []:
        _reader.detach()
        return JSONResponse(data, status_code=503)
    return JSONResponse(data)


@app.post("/api/command")
async def api_submit_command(req: CommandRequest):
    """Submit a telecontrol/setting command to a writable point."""
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
    """Read command status for a point."""
    if not _reader.is_attached():
        await asyncio.to_thread(_reader.attach)
    if not _reader.is_attached():
        return JSONResponse({"error": "Shared memory not available"}, status_code=503)
    result = await asyncio.to_thread(_reader.read_command_status, point_id)
    if "error" in result:
        return JSONResponse(result, status_code=404)
    return JSONResponse(result)


@app.get("/api/config")
async def api_config():
    """Return device/point metadata from CSV config files."""
    devices = []
    try:
        with open(CONFIG_DIR / "device.csv", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                devices.append(row)
    except Exception:
        pass

    telemetry = []
    try:
        with open(CONFIG_DIR / "telemetry.csv", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                telemetry.append(row)
    except Exception:
        pass

    teleindication = []
    try:
        with open(CONFIG_DIR / "teleindication.csv", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                teleindication.append(row)
    except Exception:
        pass

    telecontrol = []
    try:
        with open(CONFIG_DIR / "telecontrol.csv", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                telecontrol.append(row)
    except Exception:
        pass

    setting = []
    try:
        with open(CONFIG_DIR / "setting.csv", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                setting.append(row)
    except Exception:
        pass

    return JSONResponse({
        "devices": devices,
        "telemetry": telemetry,
        "teleindication": teleindication,
        "telecontrol": telecontrol,
        "setting": setting,
    })