import csv
import shutil
from copy import deepcopy
from datetime import datetime
from pathlib import Path


DEVICE_TYPE_OPTIONS = ["PV", "BESS", "Meter", "Inverter", "Grid", "Transformer", "Unknown"]
PROTOCOL_OPTIONS = ["modbus-tcp", "iec104"]
DATA_TYPE_OPTIONS = ["bool", "int16", "uint16", "int32", "uint32", "float32", "int64", "uint64", "double"]
BOOL_OPTIONS = ["false", "true"]
LOG_LEVEL_OPTIONS = ["trace", "debug", "info", "warn", "error", "fatal"]
ALARM_OPERATOR_OPTIONS = ["<", "<=", ">", ">=", "==", "!="]
ALARM_SEVERITY_OPTIONS = ["info", "warning", "critical"]
TOPOLOGY_NODE_TYPE_OPTIONS = ["grid", "bus", "breaker", "switch", "pv", "bess", "load", "meter", "transformer", "unknown"]
TOPOLOGY_LINK_TYPE_OPTIONS = ["line", "breaker", "transformer", "virtual"]
TOPOLOGY_TARGET_TYPE_OPTIONS = ["node", "link"]
TOPOLOGY_BINDING_ROLE_OPTIONS = ["status", "power", "voltage", "current", "soc", "control", "alarm"]


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
        "name": "alarm_rule",
        "file": "alarm_rule.csv",
        "title": "Alarm Rules",
        "single_row": False,
        "optional": True,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Rule ID", "required": True, "type": "text"},
            {"name": "point_id", "label": "Point ID", "required": True, "type": "select", "dynamic_options": "point_ids"},
            {"name": "enabled", "label": "Enabled", "required": True, "type": "select", "options": BOOL_OPTIONS},
            {"name": "operator", "label": "Operator", "required": True, "type": "select", "options": ALARM_OPERATOR_OPTIONS},
            {"name": "threshold", "label": "Threshold", "required": True, "type": "number"},
            {"name": "severity", "label": "Severity", "required": True, "type": "select", "options": ALARM_SEVERITY_OPTIONS},
            {"name": "message", "label": "Message", "required": True, "type": "text"},
        ],
    },
    {
        "name": "topology_node",
        "file": "topology_node.csv",
        "title": "Topology Nodes",
        "single_row": False,
        "optional": True,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Node ID", "required": True, "type": "text"},
            {"name": "site_id", "label": "Site ID", "required": True, "type": "text"},
            {"name": "name", "label": "Name", "required": True, "type": "text"},
            {"name": "type", "label": "Type", "required": True, "type": "select", "options": TOPOLOGY_NODE_TYPE_OPTIONS},
            {"name": "device_id", "label": "Device ID", "required": False, "type": "select", "dynamic_options": "device_ids"},
            {"name": "x", "label": "X", "required": True, "type": "number"},
            {"name": "y", "label": "Y", "required": True, "type": "number"},
            {"name": "enabled", "label": "Enabled", "required": True, "type": "select", "options": BOOL_OPTIONS},
        ],
    },
    {
        "name": "topology_link",
        "file": "topology_link.csv",
        "title": "Topology Links",
        "single_row": False,
        "optional": True,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Link ID", "required": True, "type": "text"},
            {"name": "site_id", "label": "Site ID", "required": True, "type": "text"},
            {"name": "source_node_id", "label": "Source Node", "required": True, "type": "select", "dynamic_options": "topology_node_ids"},
            {"name": "target_node_id", "label": "Target Node", "required": True, "type": "select", "dynamic_options": "topology_node_ids"},
            {"name": "type", "label": "Type", "required": True, "type": "select", "options": TOPOLOGY_LINK_TYPE_OPTIONS},
            {"name": "enabled", "label": "Enabled", "required": True, "type": "select", "options": BOOL_OPTIONS},
        ],
    },
    {
        "name": "topology_binding",
        "file": "topology_binding.csv",
        "title": "Topology Bindings",
        "single_row": False,
        "optional": True,
        "primary_key": "id",
        "columns": [
            {"name": "id", "label": "Binding ID", "required": True, "type": "text"},
            {"name": "target_type", "label": "Target Type", "required": True, "type": "select", "options": TOPOLOGY_TARGET_TYPE_OPTIONS},
            {"name": "target_id", "label": "Target ID", "required": True, "type": "select", "dynamic_options": "topology_target_ids"},
            {"name": "point_id", "label": "Point ID", "required": True, "type": "select", "dynamic_options": "point_ids"},
            {"name": "role", "label": "Role", "required": True, "type": "select", "options": TOPOLOGY_BINDING_ROLE_OPTIONS},
            {"name": "label", "label": "Label", "required": False, "type": "text"},
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

MODBUS_READ_FUNCTION_CODES = {"1", "2", "3", "4"}
MODBUS_WRITE_FUNCTION_CODES = {"5", "6", "16"}
DATA_TYPE_REGISTER_COUNTS = {
    "bool": 1,
    "int16": 1,
    "uint16": 1,
    "int32": 2,
    "uint32": 2,
    "float32": 2,
    "int64": 4,
    "uint64": 4,
    "double": 4,
}


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
    def __init__(self, config_dir: Path, backup_root: Path | None = None):
        self.config_dir = Path(config_dir)
        self.backup_root = Path(backup_root) if backup_root else Path("runtime") / "config_backups"

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
        point_table_by_id = {}
        point_data_type_by_id = {}
        point_writable_by_id = {}
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
                    point_table_by_id[point_id] = table_name
                    point_data_type_by_id[point_id] = data_type
                    point_writable_by_id[point_id] = writable

                if data_type and data_type not in DATA_TYPE_OPTIONS:
                    add_error(table_name, "data_type must be one of the supported data types.", row_index, "data_type")
                if writable not in {"true", "false"}:
                    add_error(table_name, "writable must be true or false.", row_index, "writable")
                if table_name in {"telecontrol", "teleadjust"} and writable != "true":
                    add_error(table_name, "Control and adjustment points must be writable=true.", row_index, "writable")

        alarm_rule_ids = set()
        for row_index, row in enumerate(tables["alarm_rule"]):
            rule_id = _trim(row.get("id"))
            point_id = _trim(row.get("point_id"))
            enabled = _bool_string(row.get("enabled"))
            operator = _trim(row.get("operator"))
            severity = _trim(row.get("severity"))

            if rule_id in alarm_rule_ids:
                add_error("alarm_rule", "Rule ID must be unique.", row_index, "id")
            elif rule_id:
                alarm_rule_ids.add(rule_id)
            if point_id and point_id not in point_ids:
                add_error("alarm_rule", "point_id must reference an existing point.", row_index, "point_id")
            if enabled not in {"true", "false"}:
                add_error("alarm_rule", "enabled must be true or false.", row_index, "enabled")
            if operator and operator not in ALARM_OPERATOR_OPTIONS:
                add_error("alarm_rule", "operator must be one of <, <=, >, >=, == or !=.", row_index, "operator")
            if severity and severity not in ALARM_SEVERITY_OPTIONS:
                add_error("alarm_rule", "severity must be info, warning or critical.", row_index, "severity")
            self._validate_float("alarm_rule", row_index, "threshold", row.get("threshold"), add_error)

        topology_node_ids = set()
        for row_index, row in enumerate(tables["topology_node"]):
            node_id = _trim(row.get("id"))
            site_id = _trim(row.get("site_id"))
            node_type = _trim(row.get("type"))
            device_id = _trim(row.get("device_id"))
            enabled = _bool_string(row.get("enabled"))

            if node_id in topology_node_ids:
                add_error("topology_node", "Node ID must be unique.", row_index, "id")
            elif node_id:
                topology_node_ids.add(node_id)
            if site_id and site_id not in site_ids:
                add_error("topology_node", "site_id must reference an existing site.", row_index, "site_id")
            if node_type and node_type not in TOPOLOGY_NODE_TYPE_OPTIONS:
                add_error("topology_node", "type must be one of the supported topology node types.", row_index, "type")
            if device_id and device_id not in device_ids:
                add_error("topology_node", "device_id must reference an enabled device. Leave it empty for a pure topology/test node.", row_index, "device_id")
            self._validate_float("topology_node", row_index, "x", row.get("x"), add_error)
            self._validate_float("topology_node", row_index, "y", row.get("y"), add_error)
            if enabled not in {"true", "false"}:
                add_error("topology_node", "enabled must be true or false.", row_index, "enabled")

        topology_link_ids = set()
        for row_index, row in enumerate(tables["topology_link"]):
            link_id = _trim(row.get("id"))
            site_id = _trim(row.get("site_id"))
            source_node_id = _trim(row.get("source_node_id"))
            target_node_id = _trim(row.get("target_node_id"))
            link_type = _trim(row.get("type"))
            enabled = _bool_string(row.get("enabled"))

            if link_id in topology_link_ids:
                add_error("topology_link", "Link ID must be unique.", row_index, "id")
            elif link_id:
                topology_link_ids.add(link_id)
            if site_id and site_id not in site_ids:
                add_error("topology_link", "site_id must reference an existing site.", row_index, "site_id")
            if source_node_id and source_node_id not in topology_node_ids:
                add_error("topology_link", "source_node_id must reference an existing topology node.", row_index, "source_node_id")
            if target_node_id and target_node_id not in topology_node_ids:
                add_error("topology_link", "target_node_id must reference an existing topology node.", row_index, "target_node_id")
            if source_node_id and target_node_id and source_node_id == target_node_id:
                add_error("topology_link", "source_node_id and target_node_id cannot be the same.", row_index, "target_node_id")
            if link_type and link_type not in TOPOLOGY_LINK_TYPE_OPTIONS:
                add_error("topology_link", "type must be one of the supported topology link types.", row_index, "type")
            if enabled not in {"true", "false"}:
                add_error("topology_link", "enabled must be true or false.", row_index, "enabled")

        topology_binding_ids = set()
        for row_index, row in enumerate(tables["topology_binding"]):
            binding_id = _trim(row.get("id"))
            target_type = _trim(row.get("target_type"))
            target_id = _trim(row.get("target_id"))
            point_id = _trim(row.get("point_id"))
            role = _trim(row.get("role"))

            if binding_id in topology_binding_ids:
                add_error("topology_binding", "Binding ID must be unique.", row_index, "id")
            elif binding_id:
                topology_binding_ids.add(binding_id)
            if target_type and target_type not in TOPOLOGY_TARGET_TYPE_OPTIONS:
                add_error("topology_binding", "target_type must be node or link.", row_index, "target_type")
            elif target_type == "node" and target_id and target_id not in topology_node_ids:
                add_error("topology_binding", "target_id must reference an existing topology node.", row_index, "target_id")
            elif target_type == "link" and target_id and target_id not in topology_link_ids:
                add_error("topology_binding", "target_id must reference an existing topology link.", row_index, "target_id")
            if point_id and point_id not in point_ids:
                add_error("topology_binding", "point_id must reference an existing point.", row_index, "point_id")
            if role and role not in TOPOLOGY_BINDING_ROLE_OPTIONS:
                add_error("topology_binding", "role must be one of the supported topology binding roles.", row_index, "role")

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
            point_table = point_table_by_id.get(point_id, "")
            point_data_type = point_data_type_by_id.get(point_id, "")
            register_count = _trim(row.get("register_count"))
            function_code = _trim(row.get("function_code"))
            if point_data_type and data_type and point_data_type != data_type:
                add_error("modbus_mapping", "mapping data_type must match the point data_type.", row_index, "data_type")
            expected_registers = DATA_TYPE_REGISTER_COUNTS.get(data_type)
            if expected_registers is not None and register_count:
                try:
                    parsed_count = int(register_count)
                    if parsed_count != expected_registers:
                        add_error(
                            "modbus_mapping",
                            f"register_count for {data_type} must be {expected_registers}.",
                            row_index,
                            "register_count",
                        )
                except ValueError:
                    pass
            if point_table in {"telecontrol", "teleadjust"} and function_code and function_code not in MODBUS_WRITE_FUNCTION_CODES:
                add_error("modbus_mapping", "telecontrol/teleadjust points must use write function code 5, 6 or 16.", row_index, "function_code")
            if point_table in {"telemetry", "teleindication"} and function_code and function_code not in MODBUS_READ_FUNCTION_CODES:
                add_error("modbus_mapping", "telemetry/teleindication points must use read function code 1, 2, 3 or 4.", row_index, "function_code")
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
            mapping_data_type = point_data_type_by_id.get(point_id, "")
            if mapping_data_type == "bool" and point_table_by_id.get(point_id) in {"telemetry", "teleadjust"}:
                add_error("iec104_mapping", "bool IEC104 points should normally be placed in teleindication/telecontrol tables.", row_index, "point_id")

        for point_id in sorted(modbus_points & iec104_points):
            add_error("modbus_mapping", f"Point '{point_id}' cannot have both Modbus and IEC104 mappings.")
            add_error("iec104_mapping", f"Point '{point_id}' cannot have both Modbus and IEC104 mappings.")

        return {"ok": len(errors) == 0, "errors": errors, "tables": tables}

    def save(self, payload):
        validation = self.validate(payload)
        if not validation["ok"]:
            return validation

        tables = validation["tables"]
        backup_dir = self._backup_current_csvs()
        for table in TABLE_SCHEMAS:
            columns = [column["name"] for column in table["columns"]]
            rows = [self._normalize_row(table["name"], row, columns) for row in tables[table["name"]]]
            _write_csv_rows(self.config_dir / table["file"], columns, rows)

        return {
            "ok": True,
            "message": "CSV files saved successfully.",
            "backup_dir": str(backup_dir),
            "restart_required": [
                "openems-modbus-collector",
                "openems-iec104-collector",
                "openems-alarm",
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
            if column in {"writable", "enabled"}:
                value = _bool_string(value)
            normalized[column] = value
        return normalized

    def _backup_current_csvs(self):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup_dir = self.backup_root / timestamp
        backup_dir.mkdir(parents=True, exist_ok=True)
        for table in TABLE_SCHEMAS:
            source = self.config_dir / table["file"]
            if source.exists():
                shutil.copy2(source, backup_dir / table["file"])
        return backup_dir

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
