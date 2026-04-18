"""OpenEMS Web Dashboard — FastAPI server.

Serves a real-time dashboard that reads data from the OpenEMS RtDb
shared memory and displays it as a web page.
"""

import os
import csv
import asyncio
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import HTMLResponse, JSONResponse

from shm_reader import ShmReader

WEB_DIR = Path(__file__).parent
CONFIG_DIR = Path(__file__).parent.parent.parent / "config" / "tables"

app = FastAPI(title="OpenEMS Dashboard")

# Global reader — attached once on startup
_reader = ShmReader()

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

    return JSONResponse({
        "devices": devices,
        "telemetry": telemetry,
        "teleindication": teleindication,
    })