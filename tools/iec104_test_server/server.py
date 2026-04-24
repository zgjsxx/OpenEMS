import csv
import json
import random
import threading
from collections import deque
from copy import deepcopy
from datetime import datetime
from pathlib import Path
from typing import Any, Deque, Dict, List, Optional


BASE_DIR = Path(__file__).resolve().parent
RUNTIME_DIR = BASE_DIR / "runtime"
CONFIG_PATH = RUNTIME_DIR / "config.json"
CONFIG_EXAMPLE_PATH = BASE_DIR / "config.example.json"
DEFAULT_IMPORT_CSV = BASE_DIR.parent.parent / "config" / "tables" / "iec104_mapping.csv"

SUPPORTED_TYPE_NAMES = {
    1: "M_SP_NA_1",
    3: "M_DP_NA_1",
    9: "M_ME_NA_1",
    11: "M_ME_NB_1",
    13: "M_ME_NC_1",
    45: "C_SC_NA_1",
    46: "C_DC_NA_1",
    48: "C_SE_NA_1",
    49: "C_SE_NB_1",
    50: "C_SE_NC_1",
}
MONITOR_TYPE_IDS = {1, 3, 9, 11, 13}
CONTROL_TYPE_IDS = {45, 46, 48, 49, 50}
QUALITY_NAMES = {"GOOD", "INVALID", "BLOCKED", "SUBSTITUTED", "NON_TOPICAL"}
CAUSE_NAMES = {"SPONTANEOUS", "INTERROGATED_BY_STATION", "PERIODIC", "BACKGROUND_SCAN"}


def _now_text() -> str:
    return datetime.now().strftime("%H:%M:%S")


def _to_hex(data: bytes) -> str:
    return " ".join(f"{byte:02x}" for byte in data)


def _bool_value(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    return str(value).strip().lower() in {"1", "true", "yes", "on"}


class Iec104TestServerManager:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._events: Deque[Dict[str, str]] = deque(maxlen=300)
        self._event_file = RUNTIME_DIR / "events.jsonl"
        self._server = None
        self._c104 = None
        self._running = False
        self._points_by_id: Dict[str, Any] = {}
        self._station_objects: Dict[int, Any] = {}
        self._runtime_values: Dict[str, Any] = {}
        self._runtime_point_defs: Dict[str, Dict[str, Any]] = {}
        self._config = self._load_or_init_config()
        self._runtime_values = {point["id"]: point.get("value", 0) for point in self._config.get("points", [])}

    def _load_or_init_config(self) -> Dict[str, Any]:
        RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
        if CONFIG_PATH.exists():
            return self.validate_config(json.loads(CONFIG_PATH.read_text(encoding="utf-8")))
        if CONFIG_EXAMPLE_PATH.exists():
            config = self.validate_config(json.loads(CONFIG_EXAMPLE_PATH.read_text(encoding="utf-8")))
        else:
            config = self._default_config()
        CONFIG_PATH.write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")
        return config

    @staticmethod
    def _default_config() -> Dict[str, Any]:
        return {
            "server": {
                "name": "OpenEMS IEC104 Test Server",
                "ip": "0.0.0.0",
                "port": 2404,
                "tick_rate_ms": 100,
                "ui_host": "127.0.0.1",
                "ui_port": 8094,
                "force_command_failure": False,
            },
            "stations": [
                {
                    "common_address": 1,
                    "name": "Station 1",
                }
            ],
            "points": [],
        }

    @staticmethod
    def _normalize_quality(value: Any) -> str:
        quality = str(value or "GOOD").strip().upper()
        if quality not in QUALITY_NAMES:
            raise ValueError(f"Unsupported quality: {value}")
        return quality

    @staticmethod
    def _normalize_type(type_id: int, type_name: Any) -> str:
        explicit = str(type_name or "").strip()
        if explicit:
            return explicit
        if type_id not in SUPPORTED_TYPE_NAMES:
            raise ValueError(f"Unsupported type_id: {type_id}")
        return SUPPORTED_TYPE_NAMES[type_id]

    def validate_config(self, config: Dict[str, Any]) -> Dict[str, Any]:
        if not isinstance(config, dict):
            raise ValueError("Configuration must be an object")
        server = config.get("server") or {}
        stations = config.get("stations") or []
        points = config.get("points") or []
        if not isinstance(stations, list) or not isinstance(points, list):
            raise ValueError("stations and points must be arrays")

        validated_server = {
            "name": str(server.get("name") or "OpenEMS IEC104 Test Server"),
            "ip": str(server.get("ip") or "0.0.0.0"),
            "port": int(server.get("port") or 2404),
            "tick_rate_ms": int(server.get("tick_rate_ms") or 100),
            "ui_host": str(server.get("ui_host") or "127.0.0.1"),
            "ui_port": int(server.get("ui_port") or 8094),
            "force_command_failure": _bool_value(server.get("force_command_failure", False)),
        }

        seen_ca = set()
        validated_stations = []
        for raw_station in stations:
            station = {
                "common_address": int(raw_station.get("common_address")),
                "name": str(raw_station.get("name") or f"Station {raw_station.get('common_address')}"),
            }
            if station["common_address"] <= 0:
                raise ValueError("station common_address must be > 0")
            if station["common_address"] in seen_ca:
                raise ValueError(f"Duplicate station common_address: {station['common_address']}")
            seen_ca.add(station["common_address"])
            validated_stations.append(station)

        if not validated_stations:
            raise ValueError("At least one station is required")

        seen_ids = set()
        seen_point_keys = set()
        validated_points = []
        for raw_point in points:
            point_id = str(raw_point.get("id") or "").strip()
            if not point_id:
                raise ValueError("point id is required")
            if point_id in seen_ids:
                raise ValueError(f"Duplicate point id: {point_id}")
            seen_ids.add(point_id)

            station_ca = int(raw_point.get("station_common_address"))
            if station_ca not in seen_ca:
                raise ValueError(f"Point {point_id} references unknown station common_address={station_ca}")

            type_id = int(raw_point.get("type_id"))
            if type_id not in SUPPORTED_TYPE_NAMES:
                raise ValueError(f"Point {point_id} has unsupported type_id={type_id}")

            point_type = self._normalize_type(type_id, raw_point.get("type"))
            category = str(raw_point.get("category") or "").strip().lower()
            if category not in {"telemetry", "teleindication", "telecontrol", "teleadjust"}:
                raise ValueError(f"Point {point_id} has unsupported category={category}")

            ioa = int(raw_point.get("ioa"))
            point_key = (station_ca, ioa, type_id)
            if point_key in seen_point_keys:
                raise ValueError(f"Duplicate station/ioa/type combination for point {point_id}")
            seen_point_keys.add(point_key)

            related_ioa = raw_point.get("related_ioa")
            normalized_related_ioa = None if related_ioa in (None, "") else int(related_ioa)

            validated_points.append(
                {
                    "id": point_id,
                    "station_common_address": station_ca,
                    "ioa": ioa,
                    "type_id": type_id,
                    "type": point_type,
                    "category": category,
                    "value": raw_point.get("value", 0),
                    "quality": self._normalize_quality(raw_point.get("quality", "GOOD")),
                    "report_ms": int(raw_point.get("report_ms") or 1000),
                    "auto_transmit": _bool_value(raw_point.get("auto_transmit", True)),
                    "writable": _bool_value(raw_point.get("writable", type_id in CONTROL_TYPE_IDS)),
                    "related_ioa": normalized_related_ioa,
                    "scale": float(raw_point.get("scale", 1) or 1),
                    "description": str(raw_point.get("description") or ""),
                }
            )

        return {
            "server": validated_server,
            "stations": validated_stations,
            "points": validated_points,
        }

    @staticmethod
    def _structural_signature(config: Dict[str, Any]) -> Dict[str, Any]:
        server = config.get("server", {})
        stations = sorted(
            [
                {
                    "common_address": int(station.get("common_address", 0)),
                    "name": str(station.get("name", "")),
                }
                for station in config.get("stations", [])
            ],
            key=lambda item: (item["common_address"], item["name"]),
        )
        points = sorted(
            [
                {
                    "id": str(point.get("id", "")),
                    "station_common_address": int(point.get("station_common_address", 0)),
                    "ioa": int(point.get("ioa", 0)),
                    "type_id": int(point.get("type_id", 0)),
                    "type": str(point.get("type", "")),
                    "category": str(point.get("category", "")),
                    "report_ms": int(point.get("report_ms", 0)),
                    "writable": bool(point.get("writable", False)),
                    "related_ioa": point.get("related_ioa"),
                }
                for point in config.get("points", [])
            ],
            key=lambda item: item["id"],
        )
        return {
            "server": {
                "ip": str(server.get("ip", "")),
                "port": int(server.get("port", 0)),
                "tick_rate_ms": int(server.get("tick_rate_ms", 0)),
            },
            "stations": stations,
            "points": points,
        }

    def _requires_restart(self, new_config: Dict[str, Any]) -> bool:
        return self._structural_signature(self._config) != self._structural_signature(new_config)

    def _record_event(self, category: str, message: str) -> None:
        item = {
            "time": _now_text(),
            "category": category,
            "message": message,
        }
        self._events.appendleft(item)
        try:
            self._event_file.parent.mkdir(parents=True, exist_ok=True)
            with self._event_file.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(item, ensure_ascii=False) + "\n")
        except OSError:
            pass

    def get_events(self) -> List[Dict[str, str]]:
        with self._lock:
            return list(self._events)

    def get_config(self) -> Dict[str, Any]:
        with self._lock:
            return deepcopy(self._config)

    def get_status(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "running": self._running,
                "server": deepcopy(self._config.get("server", {})),
                "point_count": len(self._config.get("points", [])),
                "station_count": len(self._config.get("stations", [])),
                "runtime_point_count": len(self._points_by_id),
            }

    def save_config(self, config: Dict[str, Any]) -> Dict[str, Any]:
        validated = self.validate_config(config)
        with self._lock:
            restart_needed = self._requires_restart(validated)
            was_running = self._running
            CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            CONFIG_PATH.write_text(json.dumps(validated, ensure_ascii=False, indent=2), encoding="utf-8")
            self._config = validated
            self._runtime_values = {
                point["id"]: validated_value
                for point, validated_value in (
                    (point, point.get("value", 0)) for point in validated.get("points", [])
                )
            }
            for point_id, running_point in self._points_by_id.items():
                if point_id in self._runtime_values:
                    self._apply_runtime_state(running_point, self._find_point_config(point_id))
        restarted = False
        if was_running and restart_needed:
            self.restart()
            restarted = True
            self._record_event("config", "Saved configuration and auto-restarted server to apply structural changes")
        else:
            self._record_event("config", f"Saved configuration to {CONFIG_PATH}")
        return {
            "config": validated,
            "restart_needed": restart_needed,
            "restarted": restarted,
        }

    def _find_point_config(self, point_id: str) -> Dict[str, Any]:
        for point in self._config.get("points", []):
            if point["id"] == point_id:
                return point
        raise KeyError(point_id)

    def import_mapping_csv(self, csv_path: Optional[str]) -> Dict[str, Any]:
        path = Path(csv_path) if csv_path else DEFAULT_IMPORT_CSV
        if not path.exists():
            raise FileNotFoundError(f"CSV not found: {path}")

        stations_by_ca: Dict[int, Dict[str, Any]] = {}
        points: List[Dict[str, Any]] = []
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                point_id = str(row.get("point_id") or "").strip()
                if not point_id:
                    continue
                type_id = int(row.get("type_id") or 0)
                common_address = int(row.get("common_address") or 1)
                ioa = int(row.get("ioa") or 0)
                scale = float(row.get("scale") or 1)
                stations_by_ca.setdefault(
                    common_address,
                    {"common_address": common_address, "name": f"Station {common_address}"},
                )
                if type_id in {1, 3}:
                    category = "teleindication"
                    default_value: Any = 0
                else:
                    category = "telemetry"
                    default_value = 0.0
                points.append(
                    {
                        "id": point_id,
                        "station_common_address": common_address,
                        "ioa": ioa,
                        "type_id": type_id,
                        "type": SUPPORTED_TYPE_NAMES.get(type_id, "M_ME_NC_1"),
                        "category": category,
                        "value": default_value,
                        "quality": "GOOD",
                        "report_ms": 1000,
                        "auto_transmit": True,
                        "writable": type_id in CONTROL_TYPE_IDS,
                        "related_ioa": None,
                        "scale": scale,
                        "description": "Imported from iec104_mapping.csv",
                    }
                )

        merged = self.get_config()
        merged["stations"] = sorted(stations_by_ca.values(), key=lambda item: item["common_address"])
        merged["points"] = points
        return self.save_config(merged)

    def _load_c104(self):
        if self._c104 is None:
            import c104

            self._c104 = c104
        return self._c104

    def _quality_enum(self, quality_name: str):
        c104 = self._load_c104()
        mapping = {
            "GOOD": c104.Quality(0),
            "INVALID": c104.Quality.Invalid,
            "BLOCKED": c104.Quality.Blocked,
            "SUBSTITUTED": c104.Quality.Substituted,
            "NON_TOPICAL": c104.Quality.NonTopical,
        }
        return mapping[quality_name]

    def _coerce_value(self, point_cfg: Dict[str, Any], value: Any) -> Any:
        type_id = point_cfg["type_id"]
        if type_id == 1:
            return bool(_bool_value(value))
        if type_id == 3:
            int_value = int(float(value))
            return max(0, min(3, int_value))
        if type_id in {9, 11, 13, 48, 49, 50}:
            return float(value)
        if type_id in {45, 46}:
            return int(float(value))
        return value

    def _apply_runtime_state(self, point: Any, point_cfg: Dict[str, Any]) -> None:
        point_id = point_cfg["id"]
        point.value = self._coerce_value(point_cfg, self._runtime_values.get(point_id, point_cfg.get("value", 0)))
        point.quality = self._quality_enum(point_cfg["quality"])

    def _on_before_read(self, point_cfg: Dict[str, Any]):
        c104 = self._load_c104()

        def callback(point: c104.Point) -> None:
            with self._lock:
                self._apply_runtime_state(point, point_cfg)

        return callback

    def _on_before_auto_transmit(self, point_cfg: Dict[str, Any]):
        c104 = self._load_c104()

        def callback(point: c104.Point) -> None:
            with self._lock:
                self._apply_runtime_state(point, point_cfg)

        return callback

    def _on_receive_control(self, point_cfg: Dict[str, Any]):
        c104 = self._load_c104()

        def callback(
            point: c104.Point,
            previous_info: c104.Information,
            incoming_info: c104.IncomingMessage,
        ) -> c104.ResponseState:
            with self._lock:
                force_fail = self._config.get("server", {}).get("force_command_failure", False)
                new_value = getattr(incoming_info, "value", None)
                old_value = self._runtime_values.get(point_cfg["id"], point_cfg.get("value", 0))
                if force_fail:
                    self._record_event(
                        "rx",
                        f"Rejected command point={point_cfg['id']} ioa={point_cfg['ioa']} old={old_value} new={new_value}",
                    )
                    return c104.ResponseState.FAILURE

                self._runtime_values[point_cfg["id"]] = new_value
                related_ioa = point_cfg.get("related_ioa")
                if related_ioa is not None:
                    for related_cfg in self._config.get("points", []):
                        if (
                            related_cfg["station_common_address"] == point_cfg["station_common_address"]
                            and related_cfg["ioa"] == related_ioa
                        ):
                            self._runtime_values[related_cfg["id"]] = new_value
                            related_point = self._points_by_id.get(related_cfg["id"])
                            if related_point is not None:
                                self._apply_runtime_state(related_point, related_cfg)
                                try:
                                    related_point.transmit(cause=c104.Cot.SPONTANEOUS)
                                except Exception:
                                    pass
                            break

                self._record_event(
                    "rx",
                    f"Accepted command point={point_cfg['id']} ioa={point_cfg['ioa']} old={old_value} new={new_value}",
                )
                return c104.ResponseState.SUCCESS

        return callback

    def _build_server(self) -> None:
        c104 = self._load_c104()
        server_cfg = self._config["server"]
        self._server = c104.Server(
            ip=server_cfg["ip"],
            port=server_cfg["port"],
            tick_rate_ms=server_cfg["tick_rate_ms"],
        )

        def on_connect(server: c104.Server, ip: str) -> bool:
            self._record_event("server", f"Client connected from {ip}")
            return True

        def on_receive_raw(server: c104.Server, data: bytes) -> None:
            self._record_event("rx", f"Received raw APDU hex=\"{_to_hex(data)}\"")

        def on_send_raw(server: c104.Server, data: bytes) -> None:
            self._record_event("tx", f"Sent raw APDU hex=\"{_to_hex(data)}\"")

        self._server.on_connect(callable=on_connect)
        self._server.on_receive_raw(callable=on_receive_raw)
        self._server.on_send_raw(callable=on_send_raw)

        self._station_objects = {}
        for station_cfg in self._config.get("stations", []):
            station = self._server.add_station(common_address=station_cfg["common_address"])
            self._station_objects[station_cfg["common_address"]] = station

        self._points_by_id = {}
        self._runtime_point_defs = {}
        for point_cfg in self._config.get("points", []):
            station = self._station_objects[point_cfg["station_common_address"]]
            point = station.add_point(
                io_address=point_cfg["ioa"],
                type=getattr(c104.Type, point_cfg["type"]),
                report_ms=point_cfg["report_ms"],
            )
            self._apply_runtime_state(point, point_cfg)
            if point_cfg["type_id"] in MONITOR_TYPE_IDS:
                point.on_before_read(callable=self._on_before_read(point_cfg))
                if point_cfg["auto_transmit"]:
                    point.on_before_auto_transmit(callable=self._on_before_auto_transmit(point_cfg))
            else:
                point.on_receive(callable=self._on_receive_control(point_cfg))
            self._points_by_id[point_cfg["id"]] = point
            self._runtime_point_defs[point_cfg["id"]] = {
                "station_common_address": point_cfg["station_common_address"],
                "ioa": point_cfg["ioa"],
                "type_id": point_cfg["type_id"],
                "type": point_cfg["type"],
            }

    def start(self) -> Dict[str, Any]:
        with self._lock:
            if self._running:
                return self.get_status()
            self._build_server()
            self._server.start()
            self._running = True
            self._record_event(
                "server",
                f"IEC104 test server started at {self._config['server']['ip']}:{self._config['server']['port']}",
            )
            return self.get_status()

    def stop(self) -> Dict[str, Any]:
        with self._lock:
            if self._server is not None:
                try:
                    self._server.stop()
                finally:
                    self._server = None
                    self._points_by_id = {}
                    self._station_objects = {}
                    self._runtime_point_defs = {}
                    self._running = False
                    self._record_event("server", "IEC104 test server stopped")
            return self.get_status()

    def restart(self) -> Dict[str, Any]:
        with self._lock:
            self.stop()
            return self.start()

    def set_point_value(self, point_id: str, value: Any, quality: Optional[str] = None) -> Dict[str, Any]:
        with self._lock:
            point_cfg = self._find_point_config(point_id)
            normalized_value = self._coerce_value(point_cfg, value)
            self._runtime_values[point_id] = normalized_value
            if quality is not None:
                point_cfg["quality"] = self._normalize_quality(quality)
            running_point = self._points_by_id.get(point_id)
            if running_point is not None:
                self._apply_runtime_state(running_point, point_cfg)
            self._record_event("point", f"Point value updated point={point_id} value={normalized_value}")
            return {
                "point_id": point_id,
                "value": normalized_value,
                "quality": point_cfg["quality"],
                "running": self._running,
                "runtime_definition": self._runtime_point_defs.get(point_id),
            }

    def transmit_point(self, point_id: str, cause: str = "SPONTANEOUS") -> Dict[str, Any]:
        with self._lock:
            if point_id not in self._points_by_id:
                raise ValueError("Point is not active in running server. Save config and start or restart the server.")
            c104 = self._load_c104()
            point_cfg = self._find_point_config(point_id)
            point = self._points_by_id[point_id]
            self._apply_runtime_state(point, point_cfg)
            cause_name = str(cause or "SPONTANEOUS").strip().upper()
            if cause_name not in CAUSE_NAMES:
                raise ValueError(f"Unsupported cause: {cause}")
            point.transmit(cause=getattr(c104.Cot, cause_name))
            self._record_event(
                "tx",
                f"Point transmitted point={point_id} ioa={point_cfg['ioa']} cause={cause_name} value={self._runtime_values.get(point_id)}",
            )
            return {
                "point_id": point_id,
                "cause": cause_name,
                "runtime_definition": self._runtime_point_defs.get(point_id),
            }

    def randomize_points(self) -> Dict[str, Any]:
        updated = 0
        with self._lock:
            for point_cfg in self._config.get("points", []):
                if point_cfg["type_id"] == 1:
                    value = random.choice([False, True])
                elif point_cfg["type_id"] == 3:
                    value = random.choice([0, 1, 2, 3])
                elif point_cfg["type_id"] in CONTROL_TYPE_IDS:
                    continue
                else:
                    value = round(random.uniform(0, 100), 3)
                self._runtime_values[point_cfg["id"]] = self._coerce_value(point_cfg, value)
                running_point = self._points_by_id.get(point_cfg["id"])
                if running_point is not None:
                    self._apply_runtime_state(running_point, point_cfg)
                updated += 1
        self._record_event("point", f"Randomized {updated} monitoring points")
        return {"updated": updated}


MANAGER = Iec104TestServerManager()
