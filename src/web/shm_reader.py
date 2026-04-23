"""Shared memory reader for OpenEMS RtDb via Win32 ctypes.

Reads the named shared memory 'Local\\openems_rt_db' created by
openems-modbus-collector, using the same fixed layout defined in
rt_db_layout.h.

Also supports command slot operations for telecontrol/teleadjust write operations.
"""

import ctypes
import ctypes.wintypes
import json
import time
from datetime import datetime, timezone

# ── Win32 API bindings ──────────────────────────────────────────────

kernel32 = ctypes.windll.kernel32

FILE_MAP_ALL_ACCESS = 0xF001F
INVALID_HANDLE_VALUE = ctypes.wintypes.HANDLE(-1).value

OpenFileMappingA = kernel32.OpenFileMappingA
OpenFileMappingA.argtypes = [ctypes.wintypes.DWORD, ctypes.wintypes.BOOL, ctypes.c_char_p]
OpenFileMappingA.restype = ctypes.wintypes.HANDLE

MapViewOfFile = kernel32.MapViewOfFile
MapViewOfFile.argtypes = [
    ctypes.wintypes.HANDLE, ctypes.wintypes.DWORD,
    ctypes.wintypes.DWORD, ctypes.wintypes.DWORD,
    ctypes.c_size_t,
]
MapViewOfFile.restype = ctypes.c_void_p

UnmapViewOfFile = kernel32.UnmapViewOfFile
UnmapViewOfFile.argtypes = [ctypes.c_void_p]
UnmapViewOfFile.restype = ctypes.wintypes.BOOL

CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [ctypes.wintypes.HANDLE]
CloseHandle.restype = ctypes.wintypes.BOOL


# ── Ctypes structs matching rt_db_layout.h ──────────────────────────

class ShmHeader(ctypes.Structure):
    _fields_ = [
        ("magic",                  ctypes.c_uint32),
        ("version",                ctypes.c_uint32),
        ("telemetry_count",        ctypes.c_uint32),
        ("teleindication_count",   ctypes.c_uint32),
        ("total_point_count",      ctypes.c_uint32),
        ("index_table_offset",     ctypes.c_uint32),
        ("telemetry_offset",       ctypes.c_uint32),
        ("teleindication_offset",  ctypes.c_uint32),
        ("command_count",          ctypes.c_uint32),
        ("command_offset",         ctypes.c_uint32),
        ("last_update_time",       ctypes.c_uint64),
        ("update_seq",             ctypes.c_uint64),
        ("site_id",                ctypes.c_char * 32),
        ("site_name",              ctypes.c_char * 64),
        ("reserved",               ctypes.c_uint32 * 2),
    ]


class PointIndexEntry(ctypes.Structure):
    _fields_ = [
        ("point_id",      ctypes.c_char * 32),
        ("device_id",     ctypes.c_char * 32),
        ("point_category", ctypes.c_uint8),
        ("data_type",     ctypes.c_uint8),
        ("writable",      ctypes.c_uint8),
        ("reserved1",     ctypes.c_uint8),
        ("slot_offset",   ctypes.c_uint32),
        ("unit",          ctypes.c_char * 8),
    ]


class TelemetrySlot(ctypes.Structure):
    _fields_ = [
        ("value",     ctypes.c_double),
        ("timestamp", ctypes.c_uint64),
        ("quality",   ctypes.c_uint8),
        ("valid",     ctypes.c_uint8),
        ("reserved",  ctypes.c_uint8 * 6),
    ]


class TeleindicationSlot(ctypes.Structure):
    _fields_ = [
        ("state_code", ctypes.c_uint16),
        ("timestamp",  ctypes.c_uint64),
        ("quality",    ctypes.c_uint8),
        ("valid",      ctypes.c_uint8),
        ("reserved",   ctypes.c_uint8 * 4),
    ]


class CommandSlot(ctypes.Structure):
    _fields_ = [
        ("point_id",       ctypes.c_char * 32),
        ("reserved1",      ctypes.c_char * 8),
        ("desired_value",  ctypes.c_double),
        ("result_value",   ctypes.c_double),
        ("submit_time",    ctypes.c_uint64),
        ("complete_time",  ctypes.c_uint64),
        ("status",         ctypes.c_uint8),
        ("error_code",     ctypes.c_uint8),
        ("reserved2",      ctypes.c_char * 6),
    ]


K_MAGIC = 0x454D5300
K_VERSION = 2

QUALITY_MAP = {0: "Good", 1: "Questionable", 2: "Bad", 3: "Invalid"}
CATEGORY_MAP = {0: "telemetry", 1: "teleindication", 2: "telecontrol", 3: "teleadjust"}
COMMAND_STATUS_MAP = {0: "Pending", 1: "Executing", 2: "Success", 3: "Failed", 4: "Idle"}


# ── Shared memory reader ────────────────────────────────────────────

class ShmReader:
    """Attaches to OpenEMS RtDb shared memory and reads snapshots."""

    def __init__(self, shm_name: str = "Local\\openems_rt_db"):
        self.shm_name = shm_name
        self.handle: ctypes.wintypes.HANDLE = None
        self.base_addr: ctypes.c_void_p = None
        self.mapped_size: int = 0
        self.header: ShmHeader = None
        self._attached = False

    def attach(self) -> bool:
        """Open and map the shared memory region."""
        handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, False, self.shm_name.encode("ascii"))
        if not handle or handle == INVALID_HANDLE_VALUE:
            return False

        # First map just the header to read layout info
        hdr_addr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, ctypes.sizeof(ShmHeader))
        if not hdr_addr:
            CloseHandle(handle)
            return False

        hdr = ShmHeader.from_address(hdr_addr)
        if hdr.magic != K_MAGIC or hdr.version != K_VERSION:
            UnmapViewOfFile(hdr_addr)
            CloseHandle(handle)
            return False

        total_size = (
            ctypes.sizeof(ShmHeader)
            + hdr.total_point_count * ctypes.sizeof(PointIndexEntry)
            + hdr.telemetry_count * ctypes.sizeof(TelemetrySlot)
            + hdr.teleindication_count * ctypes.sizeof(TeleindicationSlot)
            + hdr.command_count * ctypes.sizeof(CommandSlot)
        )
        UnmapViewOfFile(hdr_addr)

        # Map full region
        base = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, total_size)
        if not base:
            CloseHandle(handle)
            return False

        self.handle = handle
        self.base_addr = base
        self.mapped_size = total_size
        self.header = ShmHeader.from_address(base)
        self._attached = True
        return True

    def detach(self):
        if self.base_addr:
            UnmapViewOfFile(self.base_addr)
            self.base_addr = None
        if self.handle:
            CloseHandle(self.handle)
            self.handle = None
        self._attached = False

    def is_attached(self) -> bool:
        return self._attached

    def _index_table(self) -> ctypes.Array:
        addr = self.base_addr + self.header.index_table_offset
        return (PointIndexEntry * self.header.total_point_count).from_address(addr)

    def _telemetry_slots(self) -> ctypes.Array:
        addr = self.base_addr + self.header.telemetry_offset
        return (TelemetrySlot * self.header.telemetry_count).from_address(addr)

    def _teleindication_slots(self) -> ctypes.Array:
        addr = self.base_addr + self.header.teleindication_offset
        return (TeleindicationSlot * self.header.teleindication_count).from_address(addr)

    def _command_slots(self) -> ctypes.Array:
        if self.header.command_count == 0 or self.header.command_offset == 0:
            return (CommandSlot * 0)()
        addr = self.base_addr + self.header.command_offset
        return (CommandSlot * self.header.command_count).from_address(addr)

    def read_snapshot(self) -> dict:
        """Read a consistent snapshot using seqlock pattern (matching C++ code)."""
        if not self._attached:
            return {"error": "not attached", "points": []}

        for _ in range(20):  # retry up to 20 times for seqlock
            seq1 = self.header.update_seq
            if seq1 & 1:  # write in progress
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
                    continue  # empty slot

                pid = entry.point_id.decode("ascii").rstrip("\x00")
                did = entry.device_id.decode("ascii").rstrip("\x00")
                cat = entry.point_category
                unit = entry.unit.decode("ascii").rstrip("\x00")
                writable = entry.writable

                if cat == 0 or cat == 2 or cat == 3:  # telemetry / telecontrol / teleadjust
                    slot = telem_slots[telem_idx]
                    ts_ms = slot.timestamp
                    ts_str = datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime(
                        "%Y-%m-%d %H:%M:%S"
                    ) if ts_ms > 0 else "-"
                    points.append({
                        "id": pid,
                        "device": did,
                        "category": CATEGORY_MAP.get(cat, "unknown"),
                        "value": slot.value,
                        "unit": unit,
                        "quality": QUALITY_MAP.get(slot.quality, "Unknown"),
                        "valid": bool(slot.valid),
                        "writable": bool(writable),
                        "timestamp": ts_ms,
                        "timestamp_str": ts_str,
                    })
                    telem_idx += 1
                elif cat == 1:  # teleindication
                    slot = ti_slots[ti_idx]
                    ts_ms = slot.timestamp
                    ts_str = datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).strftime(
                        "%Y-%m-%d %H:%M:%S"
                    ) if ts_ms > 0 else "-"
                    points.append({
                        "id": pid,
                        "device": did,
                        "category": CATEGORY_MAP.get(cat, "unknown"),
                        "state_code": slot.state_code,
                        "unit": unit,
                        "quality": QUALITY_MAP.get(slot.quality, "Unknown"),
                        "valid": bool(slot.valid),
                        "writable": bool(writable),
                        "timestamp": ts_ms,
                        "timestamp_str": ts_str,
                    })
                    ti_idx += 1

            seq2 = self.header.update_seq
            if seq1 == seq2 and not (seq2 & 1):
                # Consistent read
                site_id = self.header.site_id.decode("ascii").rstrip("\x00")
                site_name = self.header.site_name.decode("ascii").rstrip("\x00")
                last_update_ms = self.header.last_update_time
                last_update_str = datetime.fromtimestamp(
                    last_update_ms / 1000, tz=timezone.utc
                ).strftime("%Y-%m-%d %H:%M:%S") if last_update_ms > 0 else "-"

                return {
                    "site_id": site_id,
                    "site_name": site_name,
                    "update_seq": seq2,
                    "last_update_time": last_update_ms,
                    "last_update_str": last_update_str,
                    "telemetry_count": self.header.telemetry_count,
                    "teleindication_count": self.header.teleindication_count,
                    "command_count": self.header.command_count,
                    "points": points,
                }

            # seqlock mismatch, retry
            time.sleep(0.001)

        # Failed after retries
        return {"error": "seqlock retry failed", "points": []}

    def submit_command(self, pid: str, desired_value: float) -> dict:
        """Submit a command to a writable point's command slot."""
        if not self._attached:
            return {"error": "not attached"}

        cmd_slots = self._command_slots()
        for i in range(self.header.command_count):
            slot = cmd_slots[i]
            slot_pid = slot.point_id.decode("ascii").rstrip("\x00")
            if slot_pid == pid:
                # Write directly to the command slot
                slot.desired_value = desired_value
                slot.submit_time = int(time.time() * 1000)
                slot.status = 0  # CommandPending
                slot.error_code = 0
                slot.result_value = 0.0
                slot.complete_time = 0
                return {"status": "submitted", "point_id": pid, "desired_value": desired_value}

        return {"error": "command slot not found for point: " + pid}

    def read_command_status(self, pid: str) -> dict:
        """Read the command status for a point."""
        if not self._attached:
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
