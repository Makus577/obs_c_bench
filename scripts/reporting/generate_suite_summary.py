#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import csv
import json
import os
from typing import Dict, List

try:
    import yaml  # type: ignore
except ImportError as exc:  # pragma: no cover - runtime guard
    raise SystemExit("PyYAML is required for suite summary generation.") from exc

from archive_compare_lib import (
    clean_text,
    ensure_directory,
    format_human_metric,
    humanize_duration_s,
    load_archive_row,
)


SUMMARY_COLUMNS = [
    "suite_id",
    "run_id",
    "scenario_id",
    "profile",
    "config_file",
    "op",
    "threads",
    "object_size_spec",
    "actual_duration_s",
    "success_rate_pct",
    "final_tps",
    "final_bps_bytes_per_sec",
    "avg_latency_ms",
    "p99_latency_ms",
    "avg_cpu_pct",
    "peak_cpu_pct",
    "avg_single_core_cpu_pct",
    "peak_single_core_cpu_pct",
    "avg_rss_mb",
    "peak_rss_mb",
    "perf_status",
    "longrun_status",
    "final_status",
    "final_reason",
    "report_dir",
    "log_dir",
    "exit_status",
]


def load_results_tsv(path: str) -> List[Dict[str, str]]:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        return list(reader)


def load_longrun_summary(report_dir: str) -> Dict[str, object]:
    path = os.path.join(report_dir, "longrun_summary.json")
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def load_perf_gate_summary(report_dir: str) -> Dict[str, object]:
    path = os.path.join(report_dir, "perf_gate", "gate_result.json")
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def load_suite_metadata(path: str) -> Dict[str, object]:
    with open(path, "r", encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def build_summary_rows(results: List[Dict[str, str]]) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    for result in results:
        report_dir = clean_text(result.get("report_dir"))
        archive_path = os.path.join(report_dir, "archive.csv")
        archive_row = load_archive_row(archive_path) if os.path.exists(archive_path) else {}
        longrun_row = load_longrun_summary(report_dir)
        perf_row = load_perf_gate_summary(report_dir)
        baseline_mode = clean_text(result.get("baseline_mode"))
        perf_status = clean_text(perf_row.get("overall_status"))
        if not perf_status:
            if baseline_mode == "generate":
                perf_status = "BASELINE_UPDATED"
            elif baseline_mode == "off" or baseline_mode == "":
                perf_status = ""
        elif perf_status == "PASS" and perf_row.get("warned_metrics"):
            perf_status = "WARN"

        longrun_status = clean_text(longrun_row.get("longrun_status"))
        exit_status = clean_text(result.get("exit_status"))
        final_status = "PASS"
        final_reason = ""
        if exit_status not in ("0", ""):
            final_status = "FAIL"
            final_reason = "scenario_execution_failed"
        elif perf_status in ("FAIL", "INCOMPARABLE"):
            final_status = "FAIL"
            final_reason = "baseline_check_failed"
        elif longrun_status == "FAIL":
            final_status = "FAIL"
            final_reason = "longrun_check_failed"
        elif perf_status == "WARN" or longrun_status == "WARN":
            final_status = "WARN"
            final_reason = "performance_warning" if perf_status == "WARN" else "longrun_warning"
        rows.append(
            {
                "suite_id": clean_text(result.get("suite_id")),
                "run_id": clean_text(result.get("run_id")),
                "scenario_id": clean_text(result.get("scenario_id")),
                "profile": clean_text(result.get("profile")),
                "config_file": clean_text(result.get("config_file")),
                "op": clean_text(archive_row.get("op") or result.get("op")),
                "threads": clean_text(archive_row.get("total_threads") or result.get("threads")),
                "object_size_spec": clean_text(archive_row.get("object_size_spec") or result.get("object_size_spec")),
                "actual_duration_s": clean_text(archive_row.get("actual_duration_s")),
                "success_rate_pct": clean_text(archive_row.get("success_rate_pct")),
                "final_tps": clean_text(archive_row.get("final_tps")),
                "final_bps_bytes_per_sec": clean_text(archive_row.get("final_bps_bytes_per_sec")),
                "avg_latency_ms": clean_text(archive_row.get("avg_latency_ms")),
                "p99_latency_ms": clean_text(archive_row.get("p99_latency_ms")),
                "avg_cpu_pct": clean_text(archive_row.get("avg_cpu_pct")),
                "peak_cpu_pct": clean_text(archive_row.get("peak_cpu_pct")),
                "avg_single_core_cpu_pct": clean_text(archive_row.get("avg_single_core_cpu_pct")),
                "peak_single_core_cpu_pct": clean_text(archive_row.get("peak_single_core_cpu_pct")),
                "avg_rss_mb": clean_text(archive_row.get("avg_rss_mb")),
                "peak_rss_mb": clean_text(archive_row.get("peak_rss_mb")),
                "perf_status": perf_status,
                "longrun_status": longrun_status,
                "final_status": final_status,
                "final_reason": final_reason,
                "report_dir": report_dir,
                "log_dir": clean_text(result.get("log_dir")),
                "exit_status": exit_status,
            }
        )
    return rows


def write_summary_csv(path: str, rows: List[Dict[str, object]]) -> None:
    with open(path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SUMMARY_COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_summary_md(path: str, suite_meta: Dict[str, object], rows: List[Dict[str, object]]) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# Suite Summary\n\n")
        handle.write(f"- Suite ID: `{clean_text(suite_meta.get('suite_id'))}`\n")
        handle.write(f"- Description: `{clean_text(suite_meta.get('description')) or 'N/A'}`\n")
        handle.write(f"- Scenario Count: `{len(rows)}`\n\n")
        pass_count = sum(1 for row in rows if row["final_status"] == "PASS")
        warn_count = sum(1 for row in rows if row["final_status"] == "WARN")
        fail_count = sum(1 for row in rows if row["final_status"] == "FAIL")
        baseline_updated = sum(1 for row in rows if row["perf_status"] == "BASELINE_UPDATED")
        handle.write("## Overall\n\n")
        handle.write(f"- PASS: `{pass_count}`\n")
        handle.write(f"- WARN: `{warn_count}`\n")
        handle.write(f"- FAIL: `{fail_count}`\n")
        handle.write(f"- Baseline Updated: `{baseline_updated}`\n\n")

        failed_rows = [row for row in rows if row["final_status"] == "FAIL"]
        if failed_rows:
            handle.write("## Failures\n\n")
            handle.write("| Scenario | Profile | Perf | Longrun | Reason |\n")
            handle.write("| --- | --- | --- | --- | --- |\n")
            for row in failed_rows:
                handle.write(
                    f"| `{row['scenario_id']}` | `{row['profile']}` | `{row['perf_status'] or 'N/A'}` | "
                    f"`{row['longrun_status'] or 'N/A'}` | `{row['final_reason'] or 'N/A'}` |\n"
                )
            handle.write("\n")

        handle.write("## All Scenarios\n\n")
        handle.write("| Scenario | Profile | Op | Threads | Object | Duration | TPS | BPS | Avg Single-Core CPU | Perf | Longrun | Final |\n")
        handle.write("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
        for row in rows:
            handle.write(
                f"| `{row['scenario_id']}` | `{row['profile']}` | `{row['op'] or 'N/A'}` | "
                f"`{row['threads'] or 'N/A'}` | `{row['object_size_spec'] or 'N/A'}` | "
                f"`{humanize_duration_s(float(row['actual_duration_s'])) if clean_text(row['actual_duration_s']) else 'N/A'}` | "
                f"`{row['final_tps'] or 'N/A'}` | "
                f"`{format_human_metric('final_bps_bytes_per_sec', float(row['final_bps_bytes_per_sec'])) if clean_text(row['final_bps_bytes_per_sec']) else 'N/A'}` | "
                f"`{format_human_metric('avg_single_core_cpu_pct', float(row['avg_single_core_cpu_pct'])) if clean_text(row.get('avg_single_core_cpu_pct')) else 'N/A'}` | "
                f"`{row['perf_status'] or 'N/A'}` | "
                f"`{row['longrun_status'] or 'N/A'}` | `{row['final_status']}` |\n"
            )


def write_failures_json(path: str, rows: List[Dict[str, object]]) -> None:
    failures = []
    for row in rows:
        exit_status = clean_text(row.get("exit_status"))
        perf_status = clean_text(row.get("perf_status"))
        longrun_status = clean_text(row.get("longrun_status"))
        if exit_status not in ("0", "") or longrun_status == "FAIL" or perf_status in ("FAIL", "INCOMPARABLE"):
            failures.append(row)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(failures, handle, indent=2, ensure_ascii=False)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate suite summary outputs from scenario results.")
    parser.add_argument("--suite-yaml", required=True, help="Path to resolved suite YAML")
    parser.add_argument("--scenario-results", required=True, help="Path to scenario_results.tsv")
    parser.add_argument("--output-dir", required=True, help="Suite summary output directory")
    args = parser.parse_args()

    ensure_directory(args.output_dir)
    suite_meta = load_suite_metadata(args.suite_yaml)
    rows = build_summary_rows(load_results_tsv(args.scenario_results))

    csv_path = os.path.join(args.output_dir, "suite_summary.csv")
    json_path = os.path.join(args.output_dir, "suite_summary.json")
    md_path = os.path.join(args.output_dir, "suite_summary.md")
    failures_path = os.path.join(args.output_dir, "suite_failures.json")

    write_summary_csv(csv_path, rows)
    with open(json_path, "w", encoding="utf-8") as handle:
        json.dump(rows, handle, indent=2, ensure_ascii=False)
    write_summary_md(md_path, suite_meta, rows)
    write_failures_json(failures_path, rows)

    print(f"[+] suite_summary.csv  -> {csv_path}")
    print(f"[+] suite_summary.json -> {json_path}")
    print(f"[+] suite_summary.md   -> {md_path}")
    print(f"[+] suite_failures.json -> {failures_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
