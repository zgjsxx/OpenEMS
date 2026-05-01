"""Cross-platform shared memory reader for OpenEMS RtDb V3 table space."""

from __future__ import annotations

import ctypes
import json
import mmap
import os
import time
from dataclasses import dataclass
from datetime import datetime, timezone

IS_WINDOWS = os.name == "nt"

if IS_WINDOWS:
    import ctypes.wintypes

    kernel32 = ctypes.windll.kernel32
    FILE_MAP_ALL_ACCESS = 0xF001F
    INVALID_HANDLE_VALUE = ctypes.wintypes.HANDLE(-1).value

    OpenFileMappingA = kernel32.OpenFileMappingA
    OpenFileMappingA.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.c_char_p]
    OpenFileMappingA.restype = ctypes.wintypes.HANDLE

    MapViewOfFile = kernel32.MapViewOfFile
    MapViewOfFile.argtypes = [
        ctypes.wintypes.HANDLE,
        ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD,
        ctypes.wintypes.DWORD,
        ctypes.c_size_t,
    ]
    MapViewOfFile.restype = ctypes.c_void_p

    UnmapViewOfFile = kernel32.UnmapViewOfFile
    UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    UnmapViewOfFile.restype = ctypes.wintypes.BOOL

    CloseHandle = kernel32.CloseHandle
    CloseHandle.argtypes = [ctypes.wintypes.HANDLE]
    CloseHandle.restype = ctypes.wintypes.BOOL
else:
    libc = ctypes.CDLL(None, use_errno=True)
    shm_open = libc.shm_open
    shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
    shm_open.restype = ctypes.c_int


class TableHeader(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint16),
        ("table_id", ctypes.c_uint16),
        ("header_size", ctypes.c_uint32),
        ("row_size", ctypes.c_uint32),
        ("capacity", ctypes.c_uint32),
        ("row_count", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("last_update_time", ctypes.c_uint64),
        ("update_seq", ctypes.c_uint64),
        ("table_name", ctypes.c_char * 32),
        ("reserved", ctypes.c_uint32 * 4),
    ]


class CatalogEntry(ctypes.Structure):
    _fields_ = [
        ("table_id", ctypes.c_uint16),
        ("reserved0", ctypes.c_uint16),
        ("row_size", ctypes.c_uint32),
        ("capacity", ctypes.c_uint32),
        ("row_count", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("table_name", ctypes.c_char * 32),
        ("shm_name", ctypes.c_char * 64),
    ]


class PointIndexHeader(ctypes.Structure):
    _fields_ = [
        ("base", TableHeader),
        ("telemetry_capacity", ctypes.c_uint32),
        ("teleindication_capacity", ctypes.c_uint32),
        ("command_capacity", ctypes.c_uint32),
        ("telemetry_used", ctypes.c_uint32),
        ("teleindication_used", ctypes.c_uint32),
        ("command_used", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
        ("site_id", ctypes.c_char * 32),
        ("site_name", ctypes.c_char * 64),
        ("reserved", ctypes.c_uint32 * 4),
    ]


class PointIndexEntry(ctypes.Structure):
    _fields_ = [
        ("point_id", ctypes.c_char * 32),
        ("device_id", ctypes.c_char * 32),
        ("point_category", ctypes.c_uint8),
        ("data_type", ctypes.c_uint8),
        ("writable", ctypes.c_uint8),
        ("reserved0", ctypes.c_uint8),
        ("telemetry_slot_index", ctypes.c_uint32),
        ("teleindication_slot_index", ctypes.c_uint32),
        ("command_slot_index", ctypes.c_uint32),
        ("unit", ctypes.c_char * 8),
    ]


class TelemetrySlot(ctypes.Structure):
    _fields_ = [
        ("value", ctypes.c_double),
        ("timestamp", ctypes.c_uint64),
        ("quality", ctypes.c_uint8),
        ("valid", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 6),
    ]


class TeleindicationSlot(ctypes.Structure):
    _fields_ = [
        ("state_code", ctypes.c_uint16),
        ("timestamp", ctypes.c_uint64),
        ("quality", ctypes.c_uint8),
        ("valid", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 4),
    ]


class CommandSlot(ctypes.Structure):
    _fields_ = [
        ("point_id", ctypes.c_char * 32),
        ("reserved1", ctypes.c_char * 8),
        ("desired_value", ctypes.c_double),
        ("result_value", ctypes.c_double),
        ("submit_time", ctypes.c_uint64),
        ("complete_time", ctypes.c_uint64),
        ("status", ctypes.c_uint8),
        ("error_code", ctypes.c_uint8),
        ("reserved2", ctypes.c_char * 6),
    ]


class StrategyRuntimeSlot(ctypes.Structure):
    _fields_ = [
        ("strategy_id", ctypes.c_char * 64),
        ("target_point_id", ctypes.c_char * 128),
        ("target_value", ctypes.c_double),
        ("update_time", ctypes.c_uint64),
        ("suppressed", ctypes.c_uint8),
        ("suppress_reason", ctypes.c_char * 128),
        ("last_error", ctypes.c_char * 128),
    ]


class AlarmActiveSlot(ctypes.Structure):
    _fields_ = [
        ("alarm_id", ctypes.c_char * 64),
        ("point_id", ctypes.c_char * 32),
        ("device_id", ctypes.c_char * 32),
        ("severity", ctypes.c_char * 16),
        ("message", ctypes.c_char * 128),
        ("value", ctypes.c_double),
        ("trigger_time", ctypes.c_uint64),
        ("last_update_time", ctypes.c_uint64),
        ("active", ctypes.c_uint8),
        ("unit", ctypes.c_char * 16),
    ]


K_TABLE_MAGIC = 0x454D5354
K_TABLE_VERSION = 1
K_INVALID_SLOT = 0xFFFFFFFF

TABLE_CATALOG = "catalog"
TABLE_POINT_INDEX = "point_index"
TABLE_TELEMETRY = "telemetry"
TABLE_TELEINDICATION = "teleindication"
TABLE_COMMAND = "command"
TABLE_STRATEGY_RUNTIME = "strategy_runtime"
TABLE_ALARM_ACTIVE = "alarm_active"

QUALITY_MAP = {0: "Good", 1: "Questionable", 2: "Bad", 3: "Invalid"}
CATEGORY_MAP = {0: "telemetry", 1: "teleindication", 2: "telecontrol", 3: "teleadjust"}
COMMAND_STATUS_MAP = {0: "Pending", 1: "Executing", 2: "Success", 3: "Failed", 4: "Idle"}


def _trim_fixed(value) -> str:
    raw = bytes(value)
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="ignore")


def _table_shm_name(base_name: str, table_name: str) -> str:
    return f"{base_name}_{table_name}"


def default_shm_name() -> str:
    return os.getenv("OPENEMS_SHM_NAME") or ("Local\\openems_rt_db" if IS_WINDOWS else "/openems_rt_db")


def _table_total_size(header: TableHeader) -> int:
    return int(header.header_size) + int(header.capacity) * int(header.row_size)


def _fmt_ts(ts_ms: int) -> str:
    if ts_ms <= 0:
        return "-"
    return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


@dataclass
class _AttachedTable:
    name: str
    shm_name: str
    handle: object = None
    base_addr: int | None = None
    mapped_size: int = 0
    mmap_obj: mmap.mmap | None = None
    fd: int | None = None
    header: TableHeader | None = None


class ShmReader:
    """Attaches to OpenEMS RtDb table space and reads snapshots."""

    def __init__(self, shm_name: str | None = None):
        self.shm_name = shm_name or default_shm_name()
        self.tables: dict[str, _AttachedTable] = {}
        self._attached = False

    def attach(self) -> bool:
        self.detach()
        try:
            catalog = self._attach_table_by_name(TABLE_CATALOG, _table_shm_name(self.shm_name, TABLE_CATALOG))
            self.tables[TABLE_CATALOG] = catalog
            entries = self._catalog_entries()
            for table_name in (
                TABLE_POINT_INDEX,
                TABLE_TELEMETRY,
                TABLE_TELEINDICATION,
                TABLE_COMMAND,
                TABLE_STRATEGY_RUNTIME,
                TABLE_ALARM_ACTIVE,
            ):
                entry = next((row for row in entries if row["table_name"] == table_name), None)
                if not entry:
                    return False
                self.tables[table_name] = self._attach_table_by_name(table_name, entry["shm_name"])
            self._attached = True
            return True
        except Exception:
            self.detach()
            return False

    def detach(self):
        for table in reversed(list(self.tables.values())):
            if IS_WINDOWS:
                if table.base_addr:
                    UnmapViewOfFile(table.base_addr)
                if table.handle:
                    CloseHandle(table.handle)
            else:
                if table.mmap_obj is not None:
                    table.mmap_obj.close()
                if table.fd is not None:
                    os.close(table.fd)
        self.tables.clear()
        self._attached = False

    def is_attached(self) -> bool:
        return self._attached

    def _attach_table_by_name(self, logical_name: str, shm_name: str) -> _AttachedTable:
        if IS_WINDOWS:
            handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, False, shm_name.encode("ascii"))
            if not handle or handle == INVALID_HANDLE_VALUE:
                raise RuntimeError(f"failed to open shm {shm_name}")
            hdr_addr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, ctypes.sizeof(TableHeader))
            if not hdr_addr:
                CloseHandle(handle)
                raise RuntimeError(f"failed to map header {shm_name}")
            hdr = TableHeader.from_address(hdr_addr)
            total_size = _table_total_size(hdr)
            UnmapViewOfFile(hdr_addr)
            base = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, total_size)
            if not base:
                CloseHandle(handle)
                raise RuntimeError(f"failed to map table {shm_name}")
            header = TableHeader.from_address(base)
            if header.magic != K_TABLE_MAGIC or header.version != K_TABLE_VERSION:
                UnmapViewOfFile(base)
                CloseHandle(handle)
                raise RuntimeError(f"invalid table header {shm_name}")
            return _AttachedTable(
                name=logical_name,
                shm_name=shm_name,
                handle=handle,
                base_addr=int(base),
                mapped_size=total_size,
                header=header,
            )

        fd = shm_open(shm_name.encode("ascii"), os.O_RDWR, 0o666)
        if fd < 0:
            raise RuntimeError(f"failed to open shm {shm_name}")

        try:
            hdr_map = mmap.mmap(
                fd,
                ctypes.sizeof(TableHeader),
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
            )
        except Exception as exc:
            os.close(fd)
            raise RuntimeError(f"failed to map header {shm_name}: {exc}") from exc

        try:
            hdr = TableHeader.from_buffer_copy(hdr_map)
        finally:
            hdr_map.close()

        total_size = _table_total_size(hdr)
        try:
            full_map = mmap.mmap(
                fd,
                total_size,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
            )
        except Exception as exc:
            os.close(fd)
            raise RuntimeError(f"failed to map table {shm_name}: {exc}") from exc

        base_addr = ctypes.addressof(ctypes.c_char.from_buffer(full_map))
        header = TableHeader.from_buffer(full_map)
        if header.magic != K_TABLE_MAGIC or header.version != K_TABLE_VERSION:
            full_map.close()
            os.close(fd)
            raise RuntimeError(f"invalid table header {shm_name}")

        return _AttachedTable(
            name=logical_name,
            shm_name=shm_name,
            base_addr=base_addr,
            mapped_size=total_size,
            mmap_obj=full_map,
            fd=fd,
            header=header,
        )

    def _table_header(self, table_name: str) -> TableHeader:
        table = self.tables[table_name]
        return table.header

    def _table_rows(self, table_name: str, row_type):
        table = self.tables[table_name]
        addr = table.base_addr + table.header.header_size
        return (row_type * table.header.row_count).from_address(addr)

    def _point_index_header(self) -> PointIndexHeader:
        table = self.tables[TABLE_POINT_INDEX]
        if IS_WINDOWS:
            return PointIndexHeader.from_address(table.base_addr)
        return PointIndexHeader.from_buffer(table.mmap_obj)

    def _catalog_entries(self):
        rows = self._table_rows(TABLE_CATALOG, CatalogEntry)
        data = []
        for row in rows:
          data.append(
              {
                  "table_id": row.table_id,
                  "table_name": _trim_fixed(row.table_name),
                  "shm_name": _trim_fixed(row.shm_name),
                  "row_size": row.row_size,
                  "capacity": row.capacity,
                  "row_count": row.row_count,
                  "flags": row.flags,
              }
          )
        return data

    def open_table(self, table_name: str) -> dict:
        if not self.is_attached():
            return {"error": "not attached"}
        if table_name not in self.tables:
            return {"error": f"table not found: {table_name}"}

        header = self._table_header(table_name)
        payload = {
            "table_name": table_name,
            "header": {
                "table_id": int(header.table_id),
                "row_size": int(header.row_size),
                "capacity": int(header.capacity),
                "row_count": int(header.row_count),
                "flags": int(header.flags),
                "last_update_time": int(header.last_update_time),
                "last_update_str": _fmt_ts(int(header.last_update_time)),
                "update_seq": int(header.update_seq),
            },
            "rows": [],
        }

        if table_name == TABLE_CATALOG:
            payload["rows"] = self._catalog_entries()
        elif table_name == TABLE_POINT_INDEX:
            rows = self._table_rows(TABLE_POINT_INDEX, PointIndexEntry)
            payload["rows"] = [
                {
                    "point_id": _trim_fixed(row.point_id),
                    "device_id": _trim_fixed(row.device_id),
                    "point_category": int(row.point_category),
                    "data_type": int(row.data_type),
                    "writable": bool(row.writable),
                    "telemetry_slot_index": int(row.telemetry_slot_index),
                    "teleindication_slot_index": int(row.teleindication_slot_index),
                    "command_slot_index": int(row.command_slot_index),
                    "unit": _trim_fixed(row.unit),
                }
                for row in rows
                if row.point_id[0:1] != b"\x00"
            ]
        elif table_name == TABLE_TELEMETRY:
            rows = self._table_rows(TABLE_TELEMETRY, TelemetrySlot)
            payload["rows"] = [
                {
                    "value": float(row.value),
                    "timestamp": int(row.timestamp),
                    "timestamp_str": _fmt_ts(int(row.timestamp)),
                    "quality": QUALITY_MAP.get(int(row.quality), "Unknown"),
                    "valid": bool(row.valid),
                }
                for row in rows
            ]
        elif table_name == TABLE_TELEINDICATION:
            rows = self._table_rows(TABLE_TELEINDICATION, TeleindicationSlot)
            payload["rows"] = [
                {
                    "state_code": int(row.state_code),
                    "timestamp": int(row.timestamp),
                    "timestamp_str": _fmt_ts(int(row.timestamp)),
                    "quality": QUALITY_MAP.get(int(row.quality), "Unknown"),
                    "valid": bool(row.valid),
                }
                for row in rows
            ]
        elif table_name == TABLE_COMMAND:
            rows = self._table_rows(TABLE_COMMAND, CommandSlot)
            payload["rows"] = [
                {
                    "point_id": _trim_fixed(row.point_id),
                    "desired_value": float(row.desired_value),
                    "result_value": float(row.result_value),
                    "submit_time": int(row.submit_time),
                    "submit_time_str": _fmt_ts(int(row.submit_time)),
                    "complete_time": int(row.complete_time),
                    "complete_time_str": _fmt_ts(int(row.complete_time)),
                    "status": COMMAND_STATUS_MAP.get(int(row.status), "Unknown"),
                    "error_code": int(row.error_code),
                }
                for row in rows
                if row.point_id[0:1] != b"\x00"
            ]
        elif table_name == TABLE_STRATEGY_RUNTIME:
            rows = self._table_rows(TABLE_STRATEGY_RUNTIME, StrategyRuntimeSlot)
            payload["rows"] = [
                {
                    "strategy_id": _trim_fixed(row.strategy_id),
                    "target_point_id": _trim_fixed(row.target_point_id),
                    "target_value": float(row.target_value),
                    "update_time": int(row.update_time),
                    "update_time_str": _fmt_ts(int(row.update_time)),
                    "suppressed": bool(row.suppressed),
                    "suppress_reason": _trim_fixed(row.suppress_reason),
                    "last_error": _trim_fixed(row.last_error),
                }
                for row in rows
                if row.strategy_id[0:1] != b"\x00"
            ]
        elif table_name == TABLE_ALARM_ACTIVE:
            rows = self._table_rows(TABLE_ALARM_ACTIVE, AlarmActiveSlot)
            payload["rows"] = [
                {
                    "alarm_id": _trim_fixed(row.alarm_id),
                    "point_id": _trim_fixed(row.point_id),
                    "device_id": _trim_fixed(row.device_id),
                    "severity": _trim_fixed(row.severity),
                    "message": _trim_fixed(row.message),
                    "value": float(row.value),
                    "trigger_time": int(row.trigger_time),
                    "trigger_time_str": _fmt_ts(int(row.trigger_time)),
                    "last_update_time": int(row.last_update_time),
                    "last_update_time_str": _fmt_ts(int(row.last_update_time)),
                    "active": bool(row.active),
                    "unit": _trim_fixed(row.unit),
                }
                for row in rows
                if row.alarm_id[0:1] != b"\x00"
            ]
        return payload

    def read_snapshot(self) -> dict:
        if not self.is_attached():
            return {"error": "not attached", "points": []}

        point_index_header = self._point_index_header()
        point_rows = self._table_rows(TABLE_POINT_INDEX, PointIndexEntry)
        telemetry_rows = self._table_rows(TABLE_TELEMETRY, TelemetrySlot)
        teleindication_rows = self._table_rows(TABLE_TELEINDICATION, TeleindicationSlot)

        points = []
        for entry in point_rows:
            if entry.point_id[0:1] == b"\x00":
                continue

            pid = _trim_fixed(entry.point_id)
            did = _trim_fixed(entry.device_id)
            category = int(entry.point_category)
            unit = _trim_fixed(entry.unit)
            writable = bool(entry.writable)

            if category == 1:
                if entry.teleindication_slot_index == K_INVALID_SLOT:
                    continue
                slot = teleindication_rows[entry.teleindication_slot_index]
                ts_ms = int(slot.timestamp)
                points.append(
                    {
                        "id": pid,
                        "device": did,
                        "category": CATEGORY_MAP.get(category, "unknown"),
                        "state_code": int(slot.state_code),
                        "unit": unit,
                        "quality": QUALITY_MAP.get(int(slot.quality), "Unknown"),
                        "valid": bool(slot.valid),
                        "writable": writable,
                        "timestamp": ts_ms,
                        "timestamp_str": _fmt_ts(ts_ms),
                    }
                )
            else:
                if entry.telemetry_slot_index == K_INVALID_SLOT:
                    continue
                slot = telemetry_rows[entry.telemetry_slot_index]
                ts_ms = int(slot.timestamp)
                points.append(
                    {
                        "id": pid,
                        "device": did,
                        "category": CATEGORY_MAP.get(category, "unknown"),
                        "value": float(slot.value),
                        "unit": unit,
                        "quality": QUALITY_MAP.get(int(slot.quality), "Unknown"),
                        "valid": bool(slot.valid),
                        "writable": writable,
                        "timestamp": ts_ms,
                        "timestamp_str": _fmt_ts(ts_ms),
                    }
                )

        telemetry_header = self._table_header(TABLE_TELEMETRY)
        teleindication_header = self._table_header(TABLE_TELEINDICATION)
        command_header = self._table_header(TABLE_COMMAND)
        strategy_runtime_header = self._table_header(TABLE_STRATEGY_RUNTIME)
        alarm_active_header = self._table_header(TABLE_ALARM_ACTIVE)

        return {
            "site_id": _trim_fixed(point_index_header.site_id),
            "site_name": _trim_fixed(point_index_header.site_name),
            "update_seq": max(
                int(point_index_header.base.update_seq),
                int(telemetry_header.update_seq),
                int(teleindication_header.update_seq),
                int(command_header.update_seq),
                int(strategy_runtime_header.update_seq),
                int(alarm_active_header.update_seq),
            ),
            "last_update_time": int(
                max(
                    point_index_header.base.last_update_time,
                    telemetry_header.last_update_time,
                    teleindication_header.last_update_time,
                    command_header.last_update_time,
                    strategy_runtime_header.last_update_time,
                    alarm_active_header.last_update_time,
                )
            ),
            "last_update_str": _fmt_ts(
                int(
                    max(
                        point_index_header.base.last_update_time,
                        telemetry_header.last_update_time,
                        teleindication_header.last_update_time,
                        command_header.last_update_time,
                        strategy_runtime_header.last_update_time,
                        alarm_active_header.last_update_time,
                    )
                )
            ),
            "telemetry_count": int(telemetry_header.row_count),
            "teleindication_count": int(teleindication_header.row_count),
            "command_count": int(command_header.row_count),
            "strategy_runtime_count": int(strategy_runtime_header.row_count),
            "alarm_active_count": int(alarm_active_header.row_count),
            "tables": self._catalog_entries(),
            "points": points,
        }

    def submit_command(self, pid: str, desired_value: float) -> dict:
        if not self.is_attached():
            return {"error": "not attached"}

        command_rows = self._table_rows(TABLE_COMMAND, CommandSlot)
        command_header = self._table_header(TABLE_COMMAND)
        now_ms = int(time.time() * 1000)
        for slot in command_rows:
            slot_pid = _trim_fixed(slot.point_id)
            if slot_pid != pid:
                continue
            slot.desired_value = desired_value
            slot.submit_time = now_ms
            slot.status = 0
            slot.error_code = 0
            slot.result_value = 0.0
            slot.complete_time = 0
            command_header.last_update_time = now_ms
            command_header.update_seq += 2
            return {"status": "submitted", "point_id": pid, "desired_value": desired_value}
        return {"error": "command slot not found for point: " + pid}

    def read_command_status(self, pid: str) -> dict:
        if not self.is_attached():
            return {"error": "not attached"}

        command_rows = self._table_rows(TABLE_COMMAND, CommandSlot)
        for slot in command_rows:
            slot_pid = _trim_fixed(slot.point_id)
            if slot_pid != pid:
                continue
            return {
                "point_id": pid,
                "desired_value": slot.desired_value,
                "result_value": slot.result_value,
                "submit_time": slot.submit_time,
                "complete_time": slot.complete_time,
                "status": COMMAND_STATUS_MAP.get(int(slot.status), "Unknown"),
                "status_code": int(slot.status),
                "error_code": int(slot.error_code),
            }
        return {"error": "command slot not found for point: " + pid}


if __name__ == "__main__":
    reader = ShmReader()
    if reader.attach():
        print(json.dumps(reader.read_snapshot(), indent=2, ensure_ascii=False))
    else:
        print(json.dumps({"error": f"failed to attach shared memory table space: {reader.shm_name}"}, ensure_ascii=False))
