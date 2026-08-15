#!/usr/bin/env python3
"""Summarize overnight_soak.py CSV without mutating the system."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import statistics
from typing import Any


def number(value: Any) -> float | None:
    try:
        return float(value) if value not in (None, "") else None
    except (TypeError, ValueError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=pathlib.Path)
    parser.add_argument("--json", dest="json_path", type=pathlib.Path)
    args = parser.parse_args()
    rows = list(csv.DictReader(args.csv.open(newline="", encoding="utf-8")))
    if not rows:
        raise SystemExit("soak CSV has no samples")
    process_values: dict[str, list[float]] = {}
    shm_counts = [int(row["shm_count"]) for row in rows if row.get("shm_count")]
    shm_bytes = [int(row["shm_bytes"]) for row in rows if row.get("shm_bytes")]
    for row in rows:
        try:
            stats = json.loads(row.get("process_stats", "{}"))
        except json.JSONDecodeError:
            stats = {}
        for name, value in stats.items():
            pss = number(value.get("pss")) if isinstance(value, dict) else None
            if pss is not None:
                process_values.setdefault(name, []).append(pss)
    elapsed = [number(row.get("elapsed_s")) for row in rows]
    elapsed = [x for x in elapsed if x is not None]
    duration = max(elapsed) if elapsed else 0.0
    summary: dict[str, Any] = {
        "samples": len(rows),
        "durationSeconds": round(duration, 3),
        "durationHours": round(duration / 3600.0, 4),
        "processPssKb": {},
        "shm": {
            "initialCount": shm_counts[0] if shm_counts else None,
            "finalCount": shm_counts[-1] if shm_counts else None,
            "minCount": min(shm_counts) if shm_counts else None,
            "maxCount": max(shm_counts) if shm_counts else None,
            "initialBytes": shm_bytes[0] if shm_bytes else None,
            "finalBytes": shm_bytes[-1] if shm_bytes else None,
            "minBytes": min(shm_bytes) if shm_bytes else None,
            "maxBytes": max(shm_bytes) if shm_bytes else None,
        },
    }
    for name, values in sorted(process_values.items()):
        growth = values[-1] - values[0] if len(values) > 1 else 0.0
        growth_hour = growth / (duration / 3600.0) if duration > 0 else None
        summary["processPssKb"][name] = {
            "initial": values[0],
            "final": values[-1],
            "min": min(values),
            "max": max(values),
            "median": statistics.median(values),
            "growth": growth,
            "growthPerHour": growth_hour,
        }
    output = json.dumps(summary, ensure_ascii=False, indent=2) + "\n"
    if args.json_path:
        args.json_path.write_text(output, encoding="utf-8")
    print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
