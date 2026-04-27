"""Import/export OpenEMS structured configuration between CSV and PostgreSQL."""

from __future__ import annotations

import argparse
from pathlib import Path

from config_store import ConfigStore
from db import Database


def main() -> int:
    parser = argparse.ArgumentParser(description="Import/export OpenEMS config tables.")
    parser.add_argument("--mode", choices=["import", "export"], default="import", help="import CSV to PostgreSQL or export PostgreSQL to CSV.")
    parser.add_argument("--config-dir", default="config/tables", help="CSV config directory.")
    parser.add_argument("--migrations-dir", default="", help="Migration directory. Defaults to this script's migrations folder.")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    migrations_dir = Path(args.migrations_dir) if args.migrations_dir else script_dir / "migrations"
    config_dir = Path(args.config_dir)

    db = Database(migrations_dir)
    init = db.initialize()
    if not init.get("ok"):
        print("Database initialization failed: " + str(init.get("error") or db.last_error))
        return 1

    store = ConfigStore(config_dir)
    if args.mode == "export":
        tables = db.load_structured_config()
        validation = store.validate({"tables": tables})
        if not validation["ok"]:
            print("PostgreSQL structured config validation failed:")
            for error in validation["errors"]:
                print(f"  {error}")
            return 1
        store.write_csv(validation["tables"])
        print(f"Exported structured PostgreSQL config to {config_dir}.")
        return 0

    tables = store.load()
    validation = store.validate({"tables": tables})
    if not validation["ok"]:
        print("CSV config validation failed:")
        for error in validation["errors"]:
            print(f"  {error}")
        return 1

    db.save_structured_config(validation["tables"])
    print(f"Imported {len(validation['tables'])} CSV config tables from {config_dir} to structured PostgreSQL tables.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
