"""PostgreSQL-backed admin data store for the OpenEMS web console."""

from __future__ import annotations

import hashlib
import json
import os
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _to_iso(value: Any) -> Optional[str]:
    if value is None:
        return None
    if isinstance(value, datetime):
        if value.tzinfo is None:
            value = value.replace(tzinfo=timezone.utc)
        return value.astimezone(timezone.utc).isoformat()
    return str(value)


class Database:
    def __init__(self, migrations_dir: Path, db_url: Optional[str] = None):
        self.migrations_dir = Path(migrations_dir)
        self.db_url = (db_url or os.getenv("OPENEMS_DB_URL") or "").strip()
        self.driver_name = ""
        self.driver = None
        self.available = False
        self.last_error = ""
        self._load_driver()

    def _load_driver(self) -> None:
        try:
            import psycopg  # type: ignore

            self.driver = psycopg
            self.driver_name = "psycopg"
        except ImportError:
            try:
                import psycopg2  # type: ignore

                self.driver = psycopg2
                self.driver_name = "psycopg2"
            except ImportError:
                self.driver = None
                self.driver_name = ""

        self.available = bool(self.db_url and self.driver is not None)
        if not self.db_url:
            self.last_error = "OPENEMS_DB_URL is not configured."
        elif self.driver is None:
            self.last_error = "PostgreSQL driver not installed. Install psycopg[binary] or psycopg2."

    @contextmanager
    def connect(self, autocommit: bool = True):
        if not self.available or self.driver is None:
            raise RuntimeError(self.last_error or "Database unavailable.")

        if self.driver_name == "psycopg":
            conn = self.driver.connect(self.db_url, autocommit=autocommit)
        else:
            conn = self.driver.connect(self.db_url)
            conn.autocommit = autocommit

        try:
            yield conn
        finally:
            conn.close()

    def _rows(self, cursor) -> List[Dict[str, Any]]:
        columns = [desc[0] for desc in cursor.description] if cursor.description else []
        rows = []
        for record in cursor.fetchall():
            rows.append({columns[index]: record[index] for index in range(len(columns))})
        return rows

    def execute(self, sql: str, params: Iterable[Any] = ()) -> None:
        with self.connect() as conn:
            with conn.cursor() as cursor:
                cursor.execute(sql, tuple(params))

    def fetch_one(self, sql: str, params: Iterable[Any] = ()) -> Optional[Dict[str, Any]]:
        with self.connect() as conn:
            with conn.cursor() as cursor:
                cursor.execute(sql, tuple(params))
                rows = self._rows(cursor)
                return rows[0] if rows else None

    def fetch_all(self, sql: str, params: Iterable[Any] = ()) -> List[Dict[str, Any]]:
        with self.connect() as conn:
            with conn.cursor() as cursor:
                cursor.execute(sql, tuple(params))
                return self._rows(cursor)

    def initialize(self) -> Dict[str, Any]:
        if not self.available:
            return {"ok": False, "error": self.last_error or "Database unavailable."}

        try:
            self.execute(
                """
                CREATE TABLE IF NOT EXISTS schema_migrations (
                    version TEXT PRIMARY KEY,
                    applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
                )
                """
            )
            applied = {
                row["version"]
                for row in self.fetch_all("SELECT version FROM schema_migrations ORDER BY version")
            }

            for path in sorted(self.migrations_dir.glob("*.sql")):
                if path.name in applied:
                    continue
                sql_text = path.read_text(encoding="utf-8")
                with self.connect() as conn:
                    with conn.cursor() as cursor:
                        cursor.execute(sql_text)
                        cursor.execute(
                            "INSERT INTO schema_migrations(version) VALUES (%s)",
                            (path.name,),
                        )

            self.ensure_default_admin()
            self.available = True
            self.last_error = ""
            return {"ok": True}
        except Exception as exc:
            self.available = False
            self.last_error = str(exc)
            return {"ok": False, "error": self.last_error}

    def health(self) -> Dict[str, Any]:
        if not self.available:
            return {"ok": False, "error": self.last_error or "Database unavailable."}
        try:
            row = self.fetch_one("SELECT NOW() AS now_ts")
            return {"ok": True, "server_time": _to_iso((row or {}).get("now_ts"))}
        except Exception as exc:
            self.available = False
            self.last_error = str(exc)
            return {"ok": False, "error": self.last_error}

    def save_structured_config(self, tables: Dict[str, List[Dict[str, Any]]]) -> None:
        """Persist normalized config editor tables into structured PostgreSQL tables."""
        with self.connect(autocommit=False) as conn:
            try:
                with conn.cursor() as cursor:
                    for table_name in (
                        "topology_bindings",
                        "topology_links",
                        "topology_nodes",
                        "alarm_rules",
                        "iec104_mappings",
                        "modbus_mappings",
                        "points",
                        "devices",
                        "ems_config",
                        "sites",
                    ):
                        cursor.execute(f"DELETE FROM {table_name}")

                    for row in tables.get("site", []):
                        cursor.execute(
                            "INSERT INTO sites(id, name, description) VALUES (%s, %s, %s)",
                            (row.get("id", ""), row.get("name", ""), row.get("description", "")),
                        )

                    for row in tables.get("ems_config", []):
                        cursor.execute(
                            """
                            INSERT INTO ems_config(singleton, log_level, default_poll_interval_ms, site_id, updated_at)
                            VALUES (TRUE, %s, %s, %s, NOW())
                            """,
                            (
                                row.get("log_level", "info"),
                                int(row.get("default_poll_interval_ms") or 1000),
                                row.get("site_id", ""),
                            ),
                        )

                    enabled_device_ids = {str(row.get("id", "")).strip() for row in tables.get("device", []) if str(row.get("id", "")).strip()}
                    enabled_point_ids = set()

                    for row in tables.get("device", []):
                        common_address = row.get("common_address", "")
                        cursor.execute(
                            """
                            INSERT INTO devices(
                                id, site_id, name, type, protocol, ip, port, unit_id,
                                poll_interval_ms, common_address
                            )
                            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                            """,
                            (
                                row.get("id", ""),
                                row.get("site_id", ""),
                                row.get("name", ""),
                                row.get("type", ""),
                                row.get("protocol", ""),
                                row.get("ip", ""),
                                int(row.get("port") or 0),
                                int(row.get("unit_id") or 0),
                                int(row.get("poll_interval_ms") or 0),
                                int(common_address) if str(common_address).strip() else None,
                            ),
                        )

                    for category, table_name in (
                        ("telemetry", "telemetry"),
                        ("teleindication", "teleindication"),
                        ("telecontrol", "telecontrol"),
                        ("teleadjust", "teleadjust"),
                    ):
                        for row in tables.get(table_name, []):
                            if str(row.get("device_id", "")).strip() not in enabled_device_ids:
                                continue
                            point_id = str(row.get("id", "")).strip()
                            if point_id:
                                enabled_point_ids.add(point_id)
                            cursor.execute(
                                """
                                INSERT INTO points(id, device_id, name, code, category, data_type, unit, writable)
                                VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
                                """,
                                (
                                    row.get("id", ""),
                                    row.get("device_id", ""),
                                    row.get("name", ""),
                                    row.get("code", ""),
                                    category,
                                    row.get("data_type", ""),
                                    row.get("unit", ""),
                                    str(row.get("writable", "")).lower() == "true",
                                ),
                            )

                    for row in tables.get("modbus_mapping", []):
                        if str(row.get("point_id", "")).strip() not in enabled_point_ids:
                            continue
                        cursor.execute(
                            """
                            INSERT INTO modbus_mappings(
                                point_id, function_code, register_address, register_count,
                                data_type, scale, offset_value
                            )
                            VALUES (%s, %s, %s, %s, %s, %s, %s)
                            """,
                            (
                                row.get("point_id", ""),
                                int(row.get("function_code") or 0),
                                int(row.get("register_address") or 0),
                                int(row.get("register_count") or 0),
                                row.get("data_type", ""),
                                float(row.get("scale") or 1.0),
                                float(row.get("offset") or 0.0),
                            ),
                        )

                    for row in tables.get("iec104_mapping", []):
                        if str(row.get("point_id", "")).strip() not in enabled_point_ids:
                            continue
                        cursor.execute(
                            """
                            INSERT INTO iec104_mappings(point_id, type_id, ioa, common_address, scale, cot)
                            VALUES (%s, %s, %s, %s, %s, %s)
                            """,
                            (
                                row.get("point_id", ""),
                                int(row.get("type_id") or 0),
                                int(row.get("ioa") or 0),
                                int(row.get("common_address") or 0),
                                float(row.get("scale") or 1.0),
                                int(row.get("cot") or 0),
                            ),
                        )

                    for row in tables.get("alarm_rule", []):
                        if str(row.get("point_id", "")).strip() not in enabled_point_ids:
                            continue
                        cursor.execute(
                            """
                            INSERT INTO alarm_rules(id, point_id, enabled, operator, threshold, severity, message)
                            VALUES (%s, %s, %s, %s, %s, %s, %s)
                            """,
                            (
                                row.get("id", ""),
                                row.get("point_id", ""),
                                str(row.get("enabled", "")).lower() == "true",
                                row.get("operator", ""),
                                float(row.get("threshold") or 0.0),
                                row.get("severity", ""),
                                row.get("message", ""),
                            ),
                        )

                    for row in tables.get("topology_node", []):
                        cursor.execute(
                            """
                            INSERT INTO topology_nodes(id, site_id, name, type, device_id, x, y, enabled)
                            VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
                            """,
                            (
                                row.get("id", ""),
                                row.get("site_id", ""),
                                row.get("name", ""),
                                row.get("type", ""),
                                row.get("device_id") or None,
                                float(row.get("x") or 0.0),
                                float(row.get("y") or 0.0),
                                str(row.get("enabled", "")).lower() == "true",
                            ),
                        )

                    for row in tables.get("topology_link", []):
                        cursor.execute(
                            """
                            INSERT INTO topology_links(id, site_id, source_node_id, target_node_id, type, enabled)
                            VALUES (%s, %s, %s, %s, %s, %s)
                            """,
                            (
                                row.get("id", ""),
                                row.get("site_id", ""),
                                row.get("source_node_id", ""),
                                row.get("target_node_id", ""),
                                row.get("type", ""),
                                str(row.get("enabled", "")).lower() == "true",
                            ),
                        )

                    for row in tables.get("topology_binding", []):
                        if str(row.get("point_id", "")).strip() not in enabled_point_ids:
                            continue
                        cursor.execute(
                            """
                            INSERT INTO topology_bindings(id, target_type, target_id, point_id, role, label)
                            VALUES (%s, %s, %s, %s, %s, %s)
                            """,
                            (
                                row.get("id", ""),
                                row.get("target_type", ""),
                                row.get("target_id", ""),
                                row.get("point_id", ""),
                                row.get("role", ""),
                                row.get("label", ""),
                            ),
                        )
                conn.commit()
            except Exception:
                conn.rollback()
                raise

    def load_structured_config(self) -> Dict[str, List[Dict[str, Any]]]:
        def text(value: Any) -> str:
            if value is None:
                return ""
            if isinstance(value, bool):
                return "true" if value else "false"
            return str(value)

        tables: Dict[str, List[Dict[str, Any]]] = {
            "ems_config": [],
            "site": [],
            "device": [],
            "telemetry": [],
            "teleindication": [],
            "telecontrol": [],
            "teleadjust": [],
            "alarm_rule": [],
            "topology_node": [],
            "topology_link": [],
            "topology_binding": [],
            "modbus_mapping": [],
            "iec104_mapping": [],
        }

        for row in self.fetch_all("SELECT log_level, default_poll_interval_ms, site_id FROM ems_config ORDER BY singleton LIMIT 1"):
            tables["ems_config"].append({
                "log_level": text(row.get("log_level")),
                "default_poll_interval_ms": text(row.get("default_poll_interval_ms")),
                "site_id": text(row.get("site_id")),
            })

        for row in self.fetch_all("SELECT id, name, description FROM sites ORDER BY id"):
            tables["site"].append({"id": text(row.get("id")), "name": text(row.get("name")), "description": text(row.get("description"))})

        for row in self.fetch_all("SELECT id, site_id, name, type, protocol, ip, port, unit_id, poll_interval_ms, common_address FROM devices ORDER BY id"):
            tables["device"].append({key: text(row.get(key)) for key in ("id", "site_id", "name", "type", "protocol", "ip", "port", "unit_id", "poll_interval_ms", "common_address")})

        for row in self.fetch_all("SELECT id, device_id, name, code, category, data_type, unit, writable FROM points ORDER BY category, id"):
            category = text(row.get("category"))
            if category not in {"telemetry", "teleindication", "telecontrol", "teleadjust"}:
                continue
            tables[category].append({
                "id": text(row.get("id")),
                "device_id": text(row.get("device_id")),
                "name": text(row.get("name")),
                "code": text(row.get("code")),
                "data_type": text(row.get("data_type")),
                "unit": text(row.get("unit")),
                "writable": text(row.get("writable")),
            })

        for row in self.fetch_all("SELECT point_id, function_code, register_address, register_count, data_type, scale, offset_value FROM modbus_mappings ORDER BY point_id"):
            tables["modbus_mapping"].append({
                "point_id": text(row.get("point_id")),
                "function_code": text(row.get("function_code")),
                "register_address": text(row.get("register_address")),
                "register_count": text(row.get("register_count")),
                "data_type": text(row.get("data_type")),
                "scale": text(row.get("scale")),
                "offset": text(row.get("offset_value")),
            })

        for row in self.fetch_all("SELECT point_id, type_id, ioa, common_address, scale, cot FROM iec104_mappings ORDER BY point_id"):
            tables["iec104_mapping"].append({key: text(row.get(key)) for key in ("point_id", "type_id", "ioa", "common_address", "scale", "cot")})

        for row in self.fetch_all("SELECT id, point_id, enabled, operator, threshold, severity, message FROM alarm_rules ORDER BY id"):
            tables["alarm_rule"].append({key: text(row.get(key)) for key in ("id", "point_id", "enabled", "operator", "threshold", "severity", "message")})

        for row in self.fetch_all("SELECT id, site_id, name, type, device_id, x, y, enabled FROM topology_nodes ORDER BY id"):
            tables["topology_node"].append({key: text(row.get(key)) for key in ("id", "site_id", "name", "type", "device_id", "x", "y", "enabled")})

        for row in self.fetch_all("SELECT id, site_id, source_node_id, target_node_id, type, enabled FROM topology_links ORDER BY id"):
            tables["topology_link"].append({key: text(row.get(key)) for key in ("id", "site_id", "source_node_id", "target_node_id", "type", "enabled")})

        for row in self.fetch_all("SELECT id, target_type, target_id, point_id, role, label FROM topology_bindings ORDER BY id"):
            tables["topology_binding"].append({key: text(row.get(key)) for key in ("id", "target_type", "target_id", "point_id", "role", "label")})

        return tables

    def ensure_default_admin(self) -> None:
        username = (os.getenv("OPENEMS_ADMIN_USERNAME") or "admin").strip()
        password_hash = (os.getenv("OPENEMS_ADMIN_PASSWORD_HASH") or "").strip()
        if not password_hash:
            password_hash = hashlib.pbkdf2_hmac(
                "sha256",
                b"admin123",
                b"openems-admin-seed",
                120000,
            ).hex()
            password_hash = "pbkdf2_sha256$openems-admin-seed$120000$" + password_hash

        existing = self.fetch_one("SELECT id FROM users WHERE username = %s", (username,))
        if existing:
            return

        self.execute(
            """
            INSERT INTO users(username, password_hash, role, status)
            VALUES (%s, %s, %s, %s)
            """,
            (username, password_hash, "admin", "active"),
        )

    def get_user_by_username(self, username: str) -> Optional[Dict[str, Any]]:
        return self.fetch_one(
            """
            SELECT id, username, password_hash, role, status, created_at, updated_at, last_login_at
            FROM users
            WHERE username = %s
            """,
            (username,),
        )

    def get_user_by_id(self, user_id: int) -> Optional[Dict[str, Any]]:
        return self.fetch_one(
            """
            SELECT id, username, password_hash, role, status, created_at, updated_at, last_login_at
            FROM users
            WHERE id = %s
            """,
            (user_id,),
        )

    def list_users(self) -> List[Dict[str, Any]]:
        rows = self.fetch_all(
            """
            SELECT id, username, role, status, created_at, updated_at, last_login_at
            FROM users
            ORDER BY username
            """
        )
        for row in rows:
            row["created_at"] = _to_iso(row.get("created_at"))
            row["updated_at"] = _to_iso(row.get("updated_at"))
            row["last_login_at"] = _to_iso(row.get("last_login_at"))
        return rows

    def create_user(self, username: str, password_hash: str, role: str, status: str) -> Dict[str, Any]:
        row = self.fetch_one(
            """
            INSERT INTO users(username, password_hash, role, status)
            VALUES (%s, %s, %s, %s)
            RETURNING id, username, role, status, created_at, updated_at, last_login_at
            """,
            (username, password_hash, role, status),
        )
        if not row:
            raise RuntimeError("Failed to create user.")
        row["created_at"] = _to_iso(row.get("created_at"))
        row["updated_at"] = _to_iso(row.get("updated_at"))
        row["last_login_at"] = _to_iso(row.get("last_login_at"))
        return row

    def update_user(
        self,
        user_id: int,
        role: Optional[str] = None,
        status: Optional[str] = None,
        password_hash: Optional[str] = None,
    ) -> Optional[Dict[str, Any]]:
        user = self.get_user_by_id(user_id)
        if not user:
            return None

        next_role = role or user["role"]
        next_status = status or user["status"]
        next_password_hash = password_hash or user["password_hash"]
        row = self.fetch_one(
            """
            UPDATE users
            SET role = %s,
                status = %s,
                password_hash = %s,
                updated_at = NOW()
            WHERE id = %s
            RETURNING id, username, role, status, created_at, updated_at, last_login_at
            """,
            (next_role, next_status, next_password_hash, user_id),
        )
        if not row:
            return None
        row["created_at"] = _to_iso(row.get("created_at"))
        row["updated_at"] = _to_iso(row.get("updated_at"))
        row["last_login_at"] = _to_iso(row.get("last_login_at"))
        return row

    def touch_login(self, user_id: int) -> None:
        self.execute("UPDATE users SET last_login_at = NOW(), updated_at = NOW() WHERE id = %s", (user_id,))

    def create_session(
        self,
        session_id: str,
        token_hash: str,
        user_id: int,
        client_ip: str,
        user_agent: str,
        ttl_hours: int = 12,
    ) -> Dict[str, Any]:
        expires_at = _utc_now() + timedelta(hours=ttl_hours)
        row = self.fetch_one(
            """
            INSERT INTO user_sessions(id, token_hash, user_id, client_ip, user_agent, expires_at)
            VALUES (%s, %s, %s, %s, %s, %s)
            RETURNING id, token_hash, user_id, created_at, expires_at, last_seen_at
            """,
            (session_id, token_hash, user_id, client_ip, user_agent[:255], expires_at),
        )
        if not row:
            raise RuntimeError("Failed to create session.")
        row["created_at"] = _to_iso(row.get("created_at"))
        row["expires_at"] = _to_iso(row.get("expires_at"))
        row["last_seen_at"] = _to_iso(row.get("last_seen_at"))
        return row

    def get_session(self, session_id: str) -> Optional[Dict[str, Any]]:
        return self.fetch_one(
            """
            SELECT s.id, s.token_hash, s.user_id, s.expires_at, s.last_seen_at, s.revoked_at,
                   u.username, u.role, u.status
            FROM user_sessions s
            JOIN users u ON u.id = s.user_id
            WHERE s.id = %s
            """,
            (session_id,),
        )

    def touch_session(self, session_id: str) -> None:
        self.execute("UPDATE user_sessions SET last_seen_at = NOW() WHERE id = %s", (session_id,))

    def revoke_session(self, session_id: str) -> None:
        self.execute("UPDATE user_sessions SET revoked_at = NOW() WHERE id = %s", (session_id,))

    def sync_active_alarms(self, active_alarms: List[Dict[str, Any]]) -> None:
        seen_ids = []
        for alarm in active_alarms:
            alarm_id = str(alarm.get("id") or alarm.get("alarm_id") or "").strip()
            if not alarm_id:
                continue
            seen_ids.append(alarm_id)
            self.execute(
                """
                INSERT INTO alarm_events(
                    alarm_id, point_id, device_id, severity, message, value_text,
                    active_since, last_seen_at, status
                )
                VALUES (%s, %s, %s, %s, %s, %s, %s, NOW(), %s)
                ON CONFLICT (alarm_id)
                DO UPDATE SET
                    point_id = EXCLUDED.point_id,
                    device_id = EXCLUDED.device_id,
                    severity = EXCLUDED.severity,
                    message = EXCLUDED.message,
                    value_text = EXCLUDED.value_text,
                    last_seen_at = NOW(),
                    cleared_at = NULL,
                    status = CASE
                        WHEN alarm_events.status IN ('acked', 'silenced') THEN alarm_events.status
                        ELSE 'active'
                    END
                """,
                (
                    alarm_id,
                    (alarm.get("point_id") or "").strip(),
                    (alarm.get("device_id") or "").strip(),
                    (alarm.get("level") or alarm.get("severity") or "warning").strip(),
                    (alarm.get("message") or "").strip(),
                    str(alarm.get("value_display") or alarm.get("value") or ""),
                    datetime.fromtimestamp((alarm.get("trigger_time") or 0) / 1000, tz=timezone.utc)
                    if alarm.get("trigger_time")
                    else _utc_now(),
                    "active",
                ),
            )

        if seen_ids:
            placeholders = ", ".join(["%s"] * len(seen_ids))
            self.execute(
                f"""
                UPDATE alarm_events
                SET cleared_at = NOW(),
                    status = CASE WHEN status = 'active' THEN 'cleared' ELSE status END
                WHERE cleared_at IS NULL
                  AND alarm_id NOT IN ({placeholders})
                """,
                tuple(seen_ids),
            )
        else:
            self.execute(
                """
                UPDATE alarm_events
                SET cleared_at = NOW(),
                    status = CASE WHEN status = 'active' THEN 'cleared' ELSE status END
                WHERE cleared_at IS NULL
                """
            )

    def list_alarms(
        self,
        status: Optional[str] = None,
        severity: Optional[str] = None,
        device_id: Optional[str] = None,
        limit: int = 200,
    ) -> List[Dict[str, Any]]:
        clauses = []
        params: List[Any] = []
        if status == "active":
            clauses.append("cleared_at IS NULL")
        elif status == "history":
            clauses.append("cleared_at IS NOT NULL")
        elif status:
            clauses.append("status = %s")
            params.append(status)
        if severity:
            clauses.append("severity = %s")
            params.append(severity)
        if device_id:
            clauses.append("device_id = %s")
            params.append(device_id)

        where_sql = "WHERE " + " AND ".join(clauses) if clauses else ""
        rows = self.fetch_all(
            f"""
            SELECT alarm_id, point_id, device_id, severity, message, value_text,
                   active_since, last_seen_at, cleared_at, ack_by, ack_at,
                   silenced_by, silenced_at, status
            FROM alarm_events
            {where_sql}
            ORDER BY COALESCE(last_seen_at, active_since) DESC
            LIMIT %s
            """,
            (*params, max(1, min(limit, 1000))),
        )
        for row in rows:
            for key in ("active_since", "last_seen_at", "cleared_at", "ack_at", "silenced_at"):
                row[key] = _to_iso(row.get(key))
        return rows

    def ack_alarm(self, alarm_id: str, username: str) -> Optional[Dict[str, Any]]:
        row = self.fetch_one(
            """
            UPDATE alarm_events
            SET ack_by = %s,
                ack_at = NOW(),
                status = CASE WHEN cleared_at IS NULL THEN 'acked' ELSE status END
            WHERE alarm_id = %s
            RETURNING alarm_id, status, ack_by, ack_at
            """,
            (username, alarm_id),
        )
        if row:
            row["ack_at"] = _to_iso(row.get("ack_at"))
        return row

    def silence_alarm(self, alarm_id: str, username: str) -> Optional[Dict[str, Any]]:
        row = self.fetch_one(
            """
            UPDATE alarm_events
            SET silenced_by = %s,
                silenced_at = NOW(),
                status = CASE WHEN cleared_at IS NULL THEN 'silenced' ELSE status END
            WHERE alarm_id = %s
            RETURNING alarm_id, status, silenced_by, silenced_at
            """,
            (username, alarm_id),
        )
        if row:
            row["silenced_at"] = _to_iso(row.get("silenced_at"))
        return row

    def log_audit(
        self,
        *,
        user_id: Optional[int],
        username: str,
        action: str,
        resource_type: str,
        resource_id: str,
        result: str,
        client_ip: str,
        before_json: Optional[Dict[str, Any]] = None,
        after_json: Optional[Dict[str, Any]] = None,
        details: str = "",
    ) -> None:
        self.execute(
            """
            INSERT INTO audit_logs(
                user_id, username, action, resource_type, resource_id,
                before_json, after_json, result, details, client_ip
            )
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
            """,
            (
                user_id,
                username[:64],
                action[:64],
                resource_type[:64],
                resource_id[:128],
                json.dumps(before_json, ensure_ascii=False) if before_json is not None else None,
                json.dumps(after_json, ensure_ascii=False) if after_json is not None else None,
                result[:32],
                details[:1024],
                client_ip[:128],
            ),
        )

    def list_audit_logs(self, action: str = "", username: str = "", limit: int = 200) -> List[Dict[str, Any]]:
        clauses = []
        params: List[Any] = []
        if action:
            clauses.append("action = %s")
            params.append(action)
        if username:
            clauses.append("username = %s")
            params.append(username)
        where_sql = "WHERE " + " AND ".join(clauses) if clauses else ""
        rows = self.fetch_all(
            f"""
            SELECT id, user_id, username, action, resource_type, resource_id,
                   before_json, after_json, result, details, client_ip, created_at
            FROM audit_logs
            {where_sql}
            ORDER BY created_at DESC
            LIMIT %s
            """,
            (*params, max(1, min(limit, 1000))),
        )
        for row in rows:
            row["created_at"] = _to_iso(row.get("created_at"))
        return rows
