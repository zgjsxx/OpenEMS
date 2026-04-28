"""Initialize PostgreSQL schema and optionally seed structured config from CSV."""

from __future__ import annotations

import argparse
from pathlib import Path

from config_store import ConfigStore
from db import Database


def main() -> int:
    parser = argparse.ArgumentParser(description="Initialize OpenEMS PostgreSQL schema for runtime startup.")
    parser.add_argument("--config-dir", default="config/tables", help="CSV config directory used for initial import.")
    parser.add_argument(
        "--migrations-dir",
        default="",
        help="Migration directory. Defaults to this script's migrations folder.",
    )
    parser.add_argument(
        "--sync-if-empty",
        action="store_true",
        help="Import CSV config only when structured PostgreSQL config is empty.",
    )
    parser.add_argument(
        "--force-sync",
        action="store_true",
        help="Always import CSV config into PostgreSQL after initialization.",
    )
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    migrations_dir = Path(args.migrations_dir) if args.migrations_dir else script_dir / "migrations"
    config_dir = Path(args.config_dir)

    db = Database(migrations_dir)
    init = db.initialize()
    if not init.get("ok"):
        print("Database initialization failed: " + str(init.get("error") or db.last_error))
        return 1

    should_sync = args.force_sync
    if args.sync_if_empty and not should_sync:
        row = db.fetch_one("SELECT COUNT(*) AS count FROM sites")
        count = int((row or {}).get("count") or 0)
        should_sync = count == 0

    if not should_sync:
        print("Structured PostgreSQL config already exists; skipping CSV import.")
        return 0

    store = ConfigStore(config_dir)
    tables = store.load()
    validation = store.validate({"tables": tables})
    if not validation["ok"]:
        print("CSV config validation failed:")
        for error in validation["errors"]:
            print(f"  {error}")
        return 1

    db.save_structured_config(validation["tables"])
    print(f"Imported {len(validation['tables'])} CSV config tables from {config_dir} into PostgreSQL.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
