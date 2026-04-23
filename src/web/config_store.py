import csv
from copy import deepcopy
from pathlib import Path


DEVICE_TYPE_OPTIONS = ["PV", "BESS", "Meter", "Inverter", "Grid", "Transformer", "Unknown"]
PROTOCOL_OPTIONS = ["modbus-tcp", "iec104"]
DATA_TYPE_OPTIONS = ["bool", "int16", "uint16", "int32", "uint32", "float32", "int64", "uint64", "double"]
BOOL_OPTIONS = ["false", "true"]
LOG_LEVEL_OPTIONS = ["trace", "debug", "info", "warn", "error", "fatal"]


TABLE_SCHEMAS = [
    {
        "name": "ems_config",
        "file": "ems_config.csv",
        "title": "EMS Config",
        "single_row": True,
        "optional": False,
        "primary_key": None,
        "columns": [
            {"name": "log_level", "label": "Log Level", "required": True, "type": "select", "options": LOG_LEVEL_OPTIONS},
            {"name": "default_poll_interval_ms", "label": "Default Poll Interval (ms)", "required": True, "type": "number"},
            {"name": "site_id", "label": "Site ID", "required": True, "type": "text"},
        ],
    },
    {
        "name": "site",
        "file": "site.csv",
        "title": "Site",
        "single_row": True,
        "optional": False,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Site ID", "required": True, "type": "text"},
            {"name": "name", "label": "Site Name", "required": True, "type": "text"},
            {"name": "description", "label": "Description", "required": False, "type": "text"},
        ],
    },
    {
        "name": "device",
        "file": "device.csv",
        "title": "Devices",
        "single_row": False,
        "optional": False,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Device ID", "required": True, "type": "text"},
            {"name": "site_id", "label": "Site ID", "required": True, "type": "text"},
            {"name": "name", "label": "Device Name", "required": True, "type": "text"},
            {"name": "type", "label": "Type", "required": True, "type": "select", "options": DEVICE_TYPE_OPTIONS},
            {"name": "protocol", "label": "Protocol", "required": True, "type": "select", "options": PROTOCOL_OPTIONS},
            {"name": "ip", "label": "IP", "required": True, "type": "text"},
            {"name": "port", "label": "Port", "required": True, "type": "number"},
            {"name": "unit_id", "label": "Unit ID", "required": True, "type": "number"},
            {"name": "poll_interval_ms", "label": "Poll Interval (ms)", "required": True, "type": "number"},
            {"name": "common_address", "label": "IEC104 Common Address", "required": False, "type": "number"},
        ],
    },
    {
        "name": "telemetry",
        "file": "telemetry.csv",
        "title": "Telemetry",
        "single_row": False,
        "optional": False,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Point ID", "required": True, "type": "text"},
            {"name": "device_id", "label": "Device ID", "required": True, "type": "select", "dynamic_options": "device_ids"},
            {"name": "name", "label": "Name", "required": True, "type": "text"},
            {"name": "code", "label": "Code", "required": False, "type": "text"},
            {"name": "data_type", "label": "Data Type", "required": True, "type": "select", "options": DATA_TYPE_OPTIONS},
            {"name": "unit", "label": "Unit", "required": False, "type": "text"},
            {"name": "writable", "label": "Writable", "required": True, "type": "select", "options": BOOL_OPTIONS},
        ],
    },
    {
        "name": "teleindication",
        "file": "teleindication.csv",
        "title": "Teleindication",
        "single_row": False,
        "optional": False,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Point ID", "required": True, "type": "text"},
            {"name": "device_id", "label": "Device ID", "required": True, "type": "select", "dynamic_options": "device_ids"},
            {"name": "name", "label": "Name", "required": True, "type": "text"},
            {"name": "code", "label": "Code", "required": False, "type": "text"},
            {"name": "data_type", "label": "Data Type", "required": True, "type": "select", "options": DATA_TYPE_OPTIONS},
            {"name": "unit", "label": "Unit", "required": False, "type": "text"},
            {"name": "writable", "label": "Writable", "required": True, "type": "select", "options": BOOL_OPTIONS},
        ],
    },
    {
        "name": "telecontrol",
        "file": "telecontrol.csv",
        "title": "Telecontrol",
        "single_row": False,
        "optional": True,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Point ID", "required": True, "type": "text"},
            {"name": "device_id", "label": "Device ID", "required": True, "type": "select", "dynamic_options": "device_ids"},
            {"name": "name", "label": "Name", "required": True, "type": "text"},
            {"name": "code", "label": "Code", "required": False, "type": "text"},
            {"name": "data_type", "label": "Data Type", "required": True, "type": "select", "options": DATA_TYPE_OPTIONS},
            {"name": "unit", "label": "Unit", "required": False, "type": "text"},
            {"name": "writable", "label": "Writable", "required": True, "type": "select", "options": BOOL_OPTIONS},
        ],
    },
    {
        "name": "teleadjust",
        "file": "teleadjust.csv",
        "title": "Teleadjust",
        "single_row": False,
        "optional": True,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Point ID", "required": True, "type": "text"},
            {"name": "device_id", "label": "Device ID", "required": True, "type": "select", "dynamic_options": "device_ids"},
            {"name": "name", "label": "Name", "required": True, "type": "text"},
            {"name": "code", "label": "Code", "required": False, "type": "text"},
            {"name": "data_type", "label": "Data Type", "required": True, "type": "select", "options": DATA_TYPE_OPTIONS},
            {"name": "unit", "label": "Unit", "required": False, "type": "text"},
            {"name": "writable", "label": "Writable", "required": True, "type": "select", "options": BOOL_OPTIONS},
        ],
    },
    {
        "name": "modbus_mapping",
        "file": "modbus_mapping.csv",
        "title": "Modbus Mapping",
        "single_row": False,
        "optional": False,
        "primary_key": "point_id",
        "columns": [
            {"name": "point_id", "label": "Point ID", "required": True, "type": "select", "dynamic_options": "modbus_point_ids"},
            {"name": "function_code", "label": "Function Code", "required": True, "type": "number"},
            {"name": "register_address", "label": "Register Address", "required": True, "type": "number"},
            {"name": "register_count", "label": "Register Count", "required": True, "type": "number"},
            {"name": "data_type", "label": "Data Type", "required": True, "type": "select", "options": DATA_TYPE_OPTIONS},
            {"name": "scale", "label": "Scale", "required": True, "type": "number"},
            {"name": "offset", "label": "Offset", "required": True, "type": "number"},
        ],
    },
    {
        "name": "iec104_mapping",
        "file": "iec104_mapping.csv",
        "title": "IEC104 Mapping",
        "single_row": False,
        "optional": False,
        "primary_key": "point_id",
        "columns": [
            {"name": "point_id", "label": "Point ID", "required": True, "type": "select", "dynamic_options": "iec104_point_ids"},
            {"name": "type_id", "label": "Type ID", "required": True, "type": "number"},
            {"name": "ioa", "label": "IOA", "required": True, "type": "number"},
            {"name": "common_address", "label": "Common Address", "required": True, "type": "number"},
            {"name": "scale", "label": "Scale", "required": True, "type": "number"},
            {"name": "cot", "label": "COT", "required": True, "type": "number"},
        ],
    },
]


SCHEMA_BY_NAME = {table["name"]: table for table in TABLE_SCHEMAS}
POINT_TABLE_NAMES = ["telemetry", "teleindication", "telecontrol", "teleadjust"]


def _trim(value):
    return str(value or "").strip()


def _read_csv_rows(path):
    if not path.exists():
        return []

    with path.open("r", encoding="utf-8", newline="") as handle:
        filtered = [line for line in handle if line.strip() and not line.lstrip().startswith("#")]
    if not filtered:
        return []

    reader = csv.DictReader(filtered)
    rows = []
    for row in reader:
        rows.append({key: _trim(value) for key, value in row.items()})
    return rows


def _write_csv_rows(path, columns, rows):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            writer.writerow({column: _trim(row.get(column, "")) for column in columns})


def _bool_string(value):
    normalized = _trim(value).lower()
    if normalized in {"1", "true"}:
        return "true"
    if normalized in {"0", "false"}:
        return "false"
    return _trim(value)


class ConfigStore:
    def __init__(self, config_dir: Path):
        self.config_dir = Path(config_dir)

    def schema(self):
        return {"tables": deepcopy(TABLE_SCHEMAS)}

    def load(self):
        data = {}
        for table in TABLE_SCHEMAS:
            data[table["name"]] = _read_csv_rows(self.config_dir / table["file"])
        return data

    def validate(self, payload):
        tables = self._normalize_payload(payload)
        errors = []

        def add_error(table, message, row=None, column=None):
            errors.append({
                "table": table,
                "row": row,
                "column": column,
                "message": message,
            })

        for table in TABLE_SCHEMAS:
            rows = tables[table["name"]]
            if table["single_row"] and len(rows) != 1:
                add_error(table["name"], f"{table['title']} must contain exactly one row.")
            for row_index, row in enumerate(rows):
                for column in table["columns"]:
                    value = _trim(row.get(column["name"], ""))
                    if column.get("required") and value == "":
                        add_error(table["name"], f"{column['label']} is required.", row_index, column["name"])

        site_rows = tables["site"]
        ems_rows = tables["ems_config"]
        site_ids = set()
        if site_rows:
            for row_index, row in enumerate(site_rows):
                site_id = _trim(row.get("id"))
                if site_id in site_ids:
                    add_error("site", "Site ID must be unique.", row_index, "id")
                elif site_id:
                    site_ids.add(site_id)
        if ems_rows:
            selected_site_id = _trim(ems_rows[0].get("site_id"))
            if selected_site_id and selected_site_id not in site_ids:
                add_error("ems_config", "site_id must reference an existing site.", 0, "site_id")

        device_ids = set()
        device_protocols = {}
        for row_index, row in enumerate(tables["device"]):
            device_id = _trim(row.get("id"))
            site_id = _trim(row.get("site_id"))
            protocol = _trim(row.get("protocol"))
            device_type = _trim(row.get("type"))

            if device_id in device_ids:
                add_error("device", "Device ID must be unique.", row_index, "id")
            elif device_id:
                device_ids.add(device_id)
                device_protocols[device_id] = protocol

            if site_id and site_id not in site_ids:
                add_error("device", "site_id must reference an existing site.", row_index, "site_id")
            if protocol and protocol not in PROTOCOL_OPTIONS:
                add_error("device", "protocol must be one of modbus-tcp or iec104.", row_index, "protocol")
            if device_type and device_type not in DEVICE_TYPE_OPTIONS:
                add_error("device", "type must be one of the supported device types.", row_index, "type")

            self._validate_int("device", row_index, "port", row.get("port"), add_error, minimum=1, maximum=65535)
            self._validate_int("device", row_index, "unit_id", row.get("unit_id"), add_error, minimum=0, maximum=255)
            self._validate_int("device", row_index, "poll_interval_ms", row.get("poll_interval_ms"), add_error, minimum=0)
            common_address = _trim(row.get("common_address"))
            if protocol == "iec104":
                self._validate_int("device", row_index, "common_address", common_address, add_error, minimum=0, maximum=65535)
            elif common_address:
                self._validate_int("device", row_index, "common_address", common_address, add_error, minimum=0, maximum=65535)

        point_ids = set()
        point_device_ids = {}
        for table_name in POINT_TABLE_NAMES:
            for row_index, row in enumerate(tables[table_name]):
                point_id = _trim(row.get("id"))
                device_id = _trim(row.get("device_id"))
                data_type = _trim(row.get("data_type"))
                writable = _bool_string(row.get("writable"))

                if point_id in point_ids:
                    add_error(table_name, "Point ID must be globally unique across all point tables.", row_index, "id")
                elif point_id:
                    point_ids.add(point_id)
                    point_device_ids[point_id] = device_id

                if device_id and device_id not in device_ids:
                    add_error(table_name, "device_id must reference an existing device.", row_index, "device_id")
                if data_type and data_type not in DATA_TYPE_OPTIONS:
                    add_error(table_name, "data_type must be one of the supported data types.", row_index, "data_type")
                if writable not in {"true", "false"}:
                    add_error(table_name, "writable must be true or false.", row_index, "writable")

        modbus_points = set()
        for row_index, row in enumerate(tables["modbus_mapping"]):
            point_id = _trim(row.get("point_id"))
            modbus_points.add(point_id)
            self._validate_point_mapping("modbus_mapping", row_index, point_id, point_ids, point_device_ids, device_protocols, "modbus-tcp", add_error)
            self._validate_int("modbus_mapping", row_index, "function_code", row.get("function_code"), add_error, minimum=0)
            self._validate_int("modbus_mapping", row_index, "register_address", row.get("register_address"), add_error, minimum=0)
            self._validate_int("modbus_mapping", row_index, "register_count", row.get("register_count"), add_error, minimum=1)
            data_type = _trim(row.get("data_type"))
            if data_type and data_type not in DATA_TYPE_OPTIONS:
                add_error("modbus_mapping", "data_type must be one of the supported data types.", row_index, "data_type")
            self._validate_float("modbus_mapping", row_index, "scale", row.get("scale"), add_error)
            self._validate_float("modbus_mapping", row_index, "offset", row.get("offset"), add_error)

        iec104_points = set()
        for row_index, row in enumerate(tables["iec104_mapping"]):
            point_id = _trim(row.get("point_id"))
            iec104_points.add(point_id)
            self._validate_point_mapping("iec104_mapping", row_index, point_id, point_ids, point_device_ids, device_protocols, "iec104", add_error)
            self._validate_int("iec104_mapping", row_index, "type_id", row.get("type_id"), add_error, minimum=0)
            self._validate_int("iec104_mapping", row_index, "ioa", row.get("ioa"), add_error, minimum=0)
            self._validate_int("iec104_mapping", row_index, "common_address", row.get("common_address"), add_error, minimum=0, maximum=65535)
            self._validate_float("iec104_mapping", row_index, "scale", row.get("scale"), add_error)
            self._validate_int("iec104_mapping", row_index, "cot", row.get("cot"), add_error, minimum=0)

        for point_id in sorted(modbus_points & iec104_points):
            add_error("modbus_mapping", f"Point '{point_id}' cannot have both Modbus and IEC104 mappings.")
            add_error("iec104_mapping", f"Point '{point_id}' cannot have both Modbus and IEC104 mappings.")

        return {"ok": len(errors) == 0, "errors": errors, "tables": tables}

    def save(self, payload):
        validation = self.validate(payload)
        if not validation["ok"]:
            return validation

        tables = validation["tables"]
        for table in TABLE_SCHEMAS:
            columns = [column["name"] for column in table["columns"]]
            rows = [self._normalize_row(table["name"], row, columns) for row in tables[table["name"]]]
            _write_csv_rows(self.config_dir / table["file"], columns, rows)

        return {
            "ok": True,
            "message": "CSV files saved successfully.",
            "restart_required": [
                "openems-modbus-collector",
                "openems-iec104-collector",
            ],
            "tables": tables,
        }

    def _normalize_payload(self, payload):
        tables = {}
        source_tables = (payload or {}).get("tables", {})
        for table in TABLE_SCHEMAS:
            rows = source_tables.get(table["name"], [])
            normalized_rows = []
            for row in rows:
                normalized_rows.append(self._normalize_row(table["name"], row, [column["name"] for column in table["columns"]]))
            tables[table["name"]] = normalized_rows
        return tables

    def _normalize_row(self, table_name, row, columns):
        normalized = {}
        for column in columns:
            value = _trim((row or {}).get(column, ""))
            if column == "writable":
                value = _bool_string(value)
            normalized[column] = value
        return normalized

    @staticmethod
    def _validate_int(table, row_index, column, value, add_error, minimum=None, maximum=None):
        text = _trim(value)
        if text == "":
            add_error(table, f"{column} is required.", row_index, column)
            return
        try:
            parsed = int(text)
        except ValueError:
            add_error(table, f"{column} must be an integer.", row_index, column)
            return
        if minimum is not None and parsed < minimum:
            add_error(table, f"{column} must be >= {minimum}.", row_index, column)
        if maximum is not None and parsed > maximum:
            add_error(table, f"{column} must be <= {maximum}.", row_index, column)

    @staticmethod
    def _validate_float(table, row_index, column, value, add_error):
        text = _trim(value)
        if text == "":
            add_error(table, f"{column} is required.", row_index, column)
            return
        try:
            float(text)
        except ValueError:
            add_error(table, f"{column} must be a number.", row_index, column)

    @staticmethod
    def _validate_point_mapping(table, row_index, point_id, point_ids, point_device_ids, device_protocols, expected_protocol, add_error):
        if point_id not in point_ids:
            add_error(table, "point_id must reference an existing point.", row_index, "point_id")
            return
        device_id = point_device_ids.get(point_id, "")
        protocol = device_protocols.get(device_id, "")
        if protocol and protocol != expected_protocol:
            add_error(
                table,
                f"Point '{point_id}' belongs to device '{device_id}' with protocol '{protocol}', expected '{expected_protocol}'.",
                row_index,
                "point_id",
            )
