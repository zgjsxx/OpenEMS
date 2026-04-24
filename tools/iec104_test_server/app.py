from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Optional

from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import uvicorn

from server import BASE_DIR, MANAGER


app = FastAPI(title="OpenEMS IEC104 Test Server")


class SaveConfigRequest(BaseModel):
    config: Dict[str, Any]


class ImportCsvRequest(BaseModel):
    csv_path: Optional[str] = None


class PointValueRequest(BaseModel):
    point_id: str
    value: Any
    quality: Optional[str] = None


class PointTransmitRequest(BaseModel):
    point_id: str
    cause: Optional[str] = "SPONTANEOUS"


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    return (BASE_DIR / "index.html").read_text(encoding="utf-8")


@app.get("/api/config")
def get_config() -> Dict[str, Any]:
    return MANAGER.get_config()


@app.post("/api/config/save")
def save_config(payload: SaveConfigRequest) -> Dict[str, Any]:
    try:
        return MANAGER.save_config(payload.config)
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/api/config/import-csv")
def import_csv(payload: ImportCsvRequest) -> Dict[str, Any]:
    try:
        return MANAGER.import_mapping_csv(payload.csv_path)
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.get("/api/status")
def status() -> Dict[str, Any]:
    return MANAGER.get_status()


@app.get("/api/events")
def events() -> Dict[str, Any]:
    return {"items": MANAGER.get_events()}


@app.post("/api/server/start")
def start_server() -> Dict[str, Any]:
    try:
        return MANAGER.start()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/api/server/stop")
def stop_server() -> Dict[str, Any]:
    try:
        return MANAGER.stop()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/api/server/restart")
def restart_server() -> Dict[str, Any]:
    try:
        return MANAGER.restart()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/api/point/value")
def set_point_value(payload: PointValueRequest) -> Dict[str, Any]:
    try:
        return MANAGER.set_point_value(payload.point_id, payload.value, payload.quality)
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/api/point/transmit")
def transmit_point(payload: PointTransmitRequest) -> Dict[str, Any]:
    try:
        return MANAGER.transmit_point(payload.point_id, payload.cause or "SPONTANEOUS")
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/api/points/randomize")
def randomize_points() -> Dict[str, Any]:
    try:
        return MANAGER.randomize_points()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


def main() -> None:
    config = MANAGER.get_config()
    server_cfg = config.get("server", {})
    uvicorn.run(
        "app:app",
        host=server_cfg.get("ui_host", "127.0.0.1"),
        port=int(server_cfg.get("ui_port", 8094)),
        reload=False,
    )


if __name__ == "__main__":
    main()

