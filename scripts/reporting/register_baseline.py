#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import csv
import os
import shutil
import sys
from datetime import datetime
from typing import Dict, List

from archive_compare_lib import clean_text, ensure_directory, infer_scenario_id, load_archive_row


MANIFEST_COLUMNS = [
    "scenario_id",
    "op",
    "threads",
    "object_size_spec",
    "users_loaded",
    "baseline_label",
    "baseline_archive_path",
    "baseline_archive_url",
    "updated_at",
    "baseline_source_run",
]


def load_manifest(path: str) -> List[Dict[str, str]]:
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def write_manifest(path: str, rows: List[Dict[str, str]]) -> None:
    ensure_directory(os.path.dirname(os.path.abspath(path)))
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=MANIFEST_COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: clean_text(row.get(key)) for key in MANIFEST_COLUMNS})


def build_manifest_row(archive_row: Dict[str, str], label: str, baseline_archive_path: str) -> Dict[str, str]:
    return {
        "scenario_id": clean_text(archive_row.get("scenario_id")) or infer_scenario_id(archive_row),
        "op": clean_text(archive_row.get("op")),
        "threads": clean_text(archive_row.get("total_threads")),
        "object_size_spec": clean_text(archive_row.get("object_size_spec")),
        "users_loaded": clean_text(archive_row.get("users_loaded")),
        "baseline_label": clean_text(label) or "auto_baseline",
        "baseline_archive_path": baseline_archive_path,
        "baseline_archive_url": "",
        "updated_at": "",
        "baseline_source_run": "",
    }


def upsert_manifest(rows: List[Dict[str, str]], new_row: Dict[str, str]) -> List[Dict[str, str]]:
    scenario_id = clean_text(new_row.get("scenario_id"))
    updated = False
    output: List[Dict[str, str]] = []
    for row in rows:
        if clean_text(row.get("scenario_id")) == scenario_id:
            output.append(new_row)
            updated = True
        else:
            output.append(row)
    if not updated:
        output.append(new_row)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description="Register or update a performance baseline from archive.csv.")
    parser.add_argument("--archive", required=True, help="Path to candidate archive.csv")
    parser.add_argument("--manifest", required=True, help="Path to baselines manifest CSV")
    parser.add_argument("--store-dir", required=True, help="Directory used to store copied baseline archive files")
    parser.add_argument("--label", default="auto_baseline", help="Baseline label written to the manifest")
    parser.add_argument("--update-strategy", default="new_label", choices=["overwrite", "new_label"], help="How to update existing baseline artifacts")
    parser.add_argument("--source-run", default="", help="Suite run id used to label the baseline version")
    args = parser.parse_args()

    try:
        archive_path = os.path.abspath(args.archive)
        manifest_path = os.path.abspath(args.manifest)
        store_dir = os.path.abspath(args.store_dir)

        archive_row = load_archive_row(archive_path)
        scenario_id = clean_text(archive_row.get("scenario_id")) or infer_scenario_id(archive_row)
        ensure_directory(store_dir)
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        version_label = clean_text(args.source_run) or timestamp
        if args.update_strategy == "overwrite":
            baseline_filename = f"{scenario_id}.csv"
        else:
            baseline_filename = f"{scenario_id}__{version_label}.csv"
        stored_archive = os.path.join(store_dir, baseline_filename)
        shutil.copyfile(archive_path, stored_archive)

        manifest_rows = load_manifest(manifest_path)
        manifest_dir = os.path.dirname(manifest_path)
        relative_archive_path = os.path.relpath(stored_archive, manifest_dir)
        new_row = build_manifest_row(archive_row, args.label, relative_archive_path)
        new_row["updated_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        new_row["baseline_source_run"] = clean_text(args.source_run)
        updated_rows = upsert_manifest(manifest_rows, new_row)
        write_manifest(manifest_path, updated_rows)

        print(f"[+] Scenario: {scenario_id}")
        print(f"[+] Update strategy: {args.update_strategy}")
        print(f"[+] Stored baseline archive -> {stored_archive}")
        print(f"[+] Updated baselines manifest -> {manifest_path}")
        return 0
    except Exception as exc:
        print(f"[-] Failed to register baseline: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
