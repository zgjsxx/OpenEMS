"""Initialize PostgreSQL schema and default data for runtime startup."""

from __future__ import annotations

import argparse
from pathlib import Path

from db import Database


def main() -> int:
    parser = argparse.ArgumentParser(description="Initialize OpenEMS PostgreSQL schema for runtime startup.")
    parser.add_argument(
        "--migrations-dir",
        default="",
        help="Migration directory. Defaults to this script's migrations folder.",
    )
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    migrations_dir = Path(args.migrations_dir) if args.migrations_dir else script_dir / "migrations"

    db = Database(migrations_dir)
    init = db.initialize()
    if not init.get("ok"):
        print("Database initialization failed: " + str(init.get("error") or db.last_error))
        return 1
    print("Database schema initialization completed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
