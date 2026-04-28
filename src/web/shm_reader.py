"""Cross-platform shared memory reader for OpenEMS RtDb."""

from __future__ import annotations

import ctypes
import json
import mmap
import os
import time
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


class ShmHeader(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("telemetry_count", ctypes.c_uint32),
        ("teleindication_count", ctypes.c_uint32),
        ("total_point_count", ctypes.c_uint32),
        ("index_table_offset", ctypes.c_uint32),
        ("telemetry_offset", ctypes.c_uint32),
        ("teleindication_offset", ctypes.c_uint32),
        ("command_count", ctypes.c_uint32),
        ("command_offset", ctypes.c_uint32),
        ("last_update_time", ctypes.c_uint64),
        ("update_seq", ctypes.c_uint64),
        ("site_id", ctypes.c_char * 32),
        ("site_name", ctypes.c_char * 64),
        ("reserved", ctypes.c_uint32 * 2),
    ]


class PointIndexEntry(ctypes.Structure):
    _fields_ = [
        ("point_id", ctypes.c_char * 32),
        ("device_id", ctypes.c_char * 32),
        ("point_category", ctypes.c_uint8),
        ("data_type", ctypes.c_uint8),
        ("writable", ctypes.c_uint8),
        ("reserved1", ctypes.c_uint8),
        ("slot_offset", ctypes.c_uint32),
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


K_MAGIC = 0x454D5300
K_VERSION = 2

QUALITY_MAP = {0: "Good", 1: "Questionable", 2: "Bad", 3: "Invalid"}
CATEGORY_MAP = {0: "telemetry", 1: "teleindication", 2: "telecontrol", 3: "teleadjust"}
COMMAND_STATUS_MAP = {0: "Pending", 1: "Executing", 2: "Success", 3: "Failed", 4: "Idle"}


def default_shm_name() -> str:
    return os.getenv("OPENEMS_SHM_NAME") or ("Local\\openems_rt_db" if IS_WINDOWS else "/openems_rt_db")


class ShmReader:
    """Attaches to OpenEMS RtDb shared memory and reads snapshots."""

    def __init__(self, shm_name: str | None = None):
        self.shm_name = shm_name or default_shm_name()
        self.handle = None
        self.base_addr: int | None = None
        self.mapped_size = 0
        self.header: ShmHeader | None = None
        self._attached = False
        self._mmap_obj: mmap.mmap | None = None
        self._fd: int | None = None

    def attach(self) -> bool:
        self.detach()
        return self._attach_windows() if IS_WINDOWS else self._attach_posix()

    def _attach_windows(self) -> bool:
        handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, False, self.shm_name.encode("ascii"))
        if not handle or handle == INVALID_HANDLE_VALUE:
            return False

        hdr_addr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, ctypes.sizeof(ShmHeader))
        if not hdr_addr:
            CloseHandle(handle)
            return False

        hdr = ShmHeader.from_address(hdr_addr)
        if hdr.magic != K_MAGIC or hdr.version != K_VERSION:
            UnmapViewOfFile(hdr_addr)
            CloseHandle(handle)
            return False

        total_size = self._layout_size(hdr)
        UnmapViewOfFile(hdr_addr)

        base = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, total_size)
        if not base:
            CloseHandle(handle)
            return False

        self.handle = handle
        self.base_addr = int(base)
        self.mapped_size = total_size
        self.header = ShmHeader.from_address(base)
        self._attached = True
        return True

    def _attach_posix(self) -> bool:
        fd = shm_open(self.shm_name.encode("ascii"), os.O_RDWR, 0o666)
        if fd < 0:
            return False

        try:
            hdr_map = mmap.mmap(
                fd,
                ctypes.sizeof(ShmHeader),
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
            )
        except Exception:
            os.close(fd)
            return False

        try:
            hdr = ShmHeader.from_buffer_copy(hdr_map)
        finally:
            hdr_map.close()

        if hdr.magic != K_MAGIC or hdr.version != K_VERSION:
            os.close(fd)
            return False

        total_size = self._layout_size(hdr)
        try:
            full_map = mmap.mmap(
                fd,
                total_size,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
            )
        except Exception:
            os.close(fd)
            return False

        self._fd = fd
        self._mmap_obj = full_map
        self.base_addr = ctypes.addressof(ctypes.c_char.from_buffer(full_map))
        self.mapped_size = total_size
        self.header = ShmHeader.from_buffer(full_map)
        self._attached = True
        return True

    @staticmethod
    def _layout_size(hdr: ShmHeader) -> int:
        return (
            ctypes.sizeof(ShmHeader)
            + hdr.total_point_count * ctypes.sizeof(PointIndexEntry)
            + hdr.telemetry_count * ctypes.sizeof(TelemetrySlot)
            + hdr.teleindication_count * ctypes.sizeof(TeleindicationSlot)
            + hdr.command_count * ctypes.sizeof(CommandSlot)
        )

    def detach(self):
        if IS_WINDOWS:
            if self.base_addr:
                UnmapViewOfFile(self.base_addr)
            if self.handle:
                CloseHandle(self.handle)
            self.handle = None
        else:
            if self._mmap_obj is not None:
                self._mmap_obj.close()
                self._mmap_obj = None
            if self._fd is not None:
                os.close(self._fd)
                self._fd = None

        self.base_addr = None
        self.mapped_size = 0
        self.header = None
        self._attached = False

    def is_attached(self) -> bool:
        return self._attached and self.base_addr is not None and self.header is not None

    def _index_table(self):
        addr = self.base_addr + self.header.index_table_offset
        return (PointIndexEntry * self.header.total_point_count).from_address(addr)

    def _telemetry_slots(self):
        addr = self.base_addr + self.header.telemetry_offset
        return (TelemetrySlot * self.header.telemetry_count).from_address(addr)

    def _teleindication_slots(self):
        addr = self.base_addr + self.header.teleindication_offset
        return (TeleindicationSlot * self.header.teleindication_count).from_address(addr)

    def _command_slots(self):
        if self.header.command_count == 0 or self.header.command_offset == 0:
            return (CommandSlot * 0)()
        addr = self.base_addr + self.header.command_offset
        return (CommandSlot * self.header.command_count).from_address(addr)

    def read_snapshot(self) -> dict:
        if not self.is_attached():
            return {"error": "not attached", "points": []}

        for _ in range(20):
            seq1 = self.header.update_seq
            if seq1 & 1:
                time.sleep(0.001)
                continue

            index = self._index_table()
            telem_slots = self._telemetry_slots()
            ti_slots = self._teleindication_slots()

            points = []
            telem_idx = 0
            ti_idx = 0

            for i in range(self.header.total_point_count):
                entry = index[i]
                if entry.point_id[0:1] == b"\x00":
                    continue

                pid = entry.point_id.decode("ascii").rstrip("\x00")
                did = entry.device_id.decode("ascii").rstrip("\x00")
                cat = entry.point_category
                unit = entry.unit.decode("ascii").rstrip("\x00")
                writable = entry.writable

                if cat in (0, 2, 3):
                    slot = telem_slots[telem_idx]
                    ts_ms = slot.timestamp
                    points.append(
                        {
                            "id": pid,
                            "device": did,
                            "category": CATEGORY_MAP.get(cat, "unknown"),
                            "value": slot.value,
                            "unit": unit,
                            "quality": QUALITY_MAP.get(slot.quality, "Unknown"),
                            "valid": bool(slot.valid),
                            "writable": bool(writable),
                            "timestamp": ts_ms,
                            "timestamp_str": _fmt_ts(ts_ms),
                        }
                    )
                    telem_idx += 1
                elif cat == 1:
                    slot = ti_slots[ti_idx]
                    ts_ms = slot.timestamp
                    points.append(
                        {
                            "id": pid,
                            "device": did,
                            "category": CATEGORY_MAP.get(cat, "unknown"),
                            "state_code": slot.state_code,
                            "unit": unit,
                            "quality": QUALITY_MAP.get(slot.quality, "Unknown"),
                            "valid": bool(slot.valid),
                            "writable": bool(writable),
                            "timestamp": ts_ms,
                            "timestamp_str": _fmt_ts(ts_ms),
                        }
                    )
                    ti_idx += 1

            seq2 = self.header.update_seq
            if seq1 == seq2 and not (seq2 & 1):
                site_id = self.header.site_id.decode("ascii").rstrip("\x00")
                site_name = self.header.site_name.decode("ascii").rstrip("\x00")
                last_update_ms = self.header.last_update_time
                return {
                    "site_id": site_id,
                    "site_name": site_name,
                    "update_seq": seq2,
                    "last_update_time": last_update_ms,
                    "last_update_str": _fmt_ts(last_update_ms),
                    "telemetry_count": self.header.telemetry_count,
                    "teleindication_count": self.header.teleindication_count,
                    "command_count": self.header.command_count,
                    "points": points,
                }

            time.sleep(0.001)

        return {"error": "seqlock retry failed", "points": []}

    def submit_command(self, pid: str, desired_value: float) -> dict:
        if not self.is_attached():
            return {"error": "not attached"}

        cmd_slots = self._command_slots()
        for i in range(self.header.command_count):
            slot = cmd_slots[i]
            slot_pid = slot.point_id.decode("ascii").rstrip("\x00")
            if slot_pid == pid:
                slot.desired_value = desired_value
                slot.submit_time = int(time.time() * 1000)
                slot.status = 0
                slot.error_code = 0
                slot.result_value = 0.0
                slot.complete_time = 0
                return {"status": "submitted", "point_id": pid, "desired_value": desired_value}

        return {"error": "command slot not found for point: " + pid}

    def read_command_status(self, pid: str) -> dict:
        if not self.is_attached():
            return {"error": "not attached"}

        cmd_slots = self._command_slots()
        for i in range(self.header.command_count):
            slot = cmd_slots[i]
            slot_pid = slot.point_id.decode("ascii").rstrip("\x00")
            if slot_pid == pid:
                return {
                    "point_id": pid,
                    "desired_value": slot.desired_value,
                    "result_value": slot.result_value,
                    "submit_time": slot.submit_time,
                    "complete_time": slot.complete_time,
                    "status": COMMAND_STATUS_MAP.get(slot.status, "Unknown"),
                    "status_code": slot.status,
                    "error_code": slot.error_code,
                }

        return {"error": "command slot not found for point: " + pid}


def _fmt_ts(ts_ms: int) -> str:
    if ts_ms <= 0:
        return "-"
    return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


if __name__ == "__main__":
    reader = ShmReader()
    if reader.attach():
        snap = reader.read_snapshot()
        print(json.dumps(snap, indent=2, ensure_ascii=False))
    else:
        print(json.dumps({"error": f"failed to attach shared memory: {reader.shm_name}"}, ensure_ascii=False))
