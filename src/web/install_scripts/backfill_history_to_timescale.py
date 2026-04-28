#!/usr/bin/env python3
"""One-time backfill script: import all existing JSONL history files into TimescaleDB.

Usage:
    python backfill_history_to_timescale.py [--history-dir PATH] [--batch-size N] [--dry-run]

This script reads every JSONL file in the history directory and bulk-inserts
the records into the history_samples hypertable. The ingestion bridge running
inside the admin portal will also automatically handle new files, but this
script offers more control for initial backfill of large existing data.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger("backfill")

BATCH_SIZE = 10000


def parse_jsonl_file(path: Path) -> List[Dict[str, Any]]:
    """Read and parse all lines from a JSONL file."""
    records: List[Dict[str, Any]] = []
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        logger.warning("Failed to read %s: %s", path, exc)
        return records

    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        records.append(record)
    return records


def main() -> None:
    parser = argparse.ArgumentParser(description="Backfill JSONL history into TimescaleDB")
    parser.add_argument(
        "--history-dir",
        default=os.getenv("OPENEMS_HISTORY_DIR", "runtime/history"),
        help="Directory containing JSONL history files",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=BATCH_SIZE,
        help="Number of records per batch insert",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Parse files but do not insert into DB",
    )
    args = parser.parse_args()

    history_dir = Path(args.history_dir)
    if not history_dir.exists():
        logger.error("History directory not found: %s", history_dir)
        sys.exit(1)

    db_url = os.getenv("OPENEMS_DB_URL", "").strip()
    if not db_url:
        logger.error("OPENEMS_DB_URL is not set")
        sys.exit(1)

    # Import db module from web directory
    web_dir = Path(__file__).resolve().parent.parent / "web"
    sys.path.insert(0, str(web_dir))
    from db import Database

    migrations_dir = web_dir / "migrations"
    db = Database(migrations_dir, db_url=db_url)
    init_result = db.initialize()
    if not init_result.get("ok"):
        logger.error("Database init failed: %s", init_result.get("error"))
        sys.exit(1)

    logger.info("Database connected. Scanning %s for JSONL files...", history_dir)
    jsonl_files = sorted(history_dir.glob("*.jsonl"))
    if not jsonl_files:
        logger.info("No JSONL files found.")
        return

    total_inserted = 0
    total_records = 0
    for path in jsonl_files:
        records = parse_jsonl_file(path)
        total_records += len(records)
        logger.info("Parsed %s: %d records", path.name, len(records))

        if not records or args.dry_run:
            if not args.dry_run:
                db.mark_ingestion_progress(
                    path.name, path.stat().st_size, len(records), 0
                )
            continue

        inserted = 0
        for i in range(0, len(records), args.batch_size):
            batch = records[i : i + args.batch_size]
            try:
                db.insert_history_batch(batch)
                inserted += len(batch)
            except Exception as exc:
                logger.error("Batch insert failed for %s: %s", path.name, exc)
                break

        total_inserted += inserted
        db.mark_ingestion_progress(
            path.name, path.stat().st_size, len(records), path.stat().st_size
        )
        logger.info("Inserted %d/%d records from %s", inserted, len(records), path.name)

    logger.info(
        "Backfill complete: %d/%d records inserted from %d files",
        total_inserted,
        total_records,
        len(jsonl_files),
    )


if __name__ == "__main__":
    main()