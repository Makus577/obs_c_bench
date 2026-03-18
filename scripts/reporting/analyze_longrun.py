#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import csv
import json
import os
import sys
from statistics import mean
from typing import Dict, List, Optional

from archive_compare_lib import (
    clean_text,
    ensure_directory,
    format_human_metric,
    humanize_duration_s,
    load_archive_row,
    load_policy_file,
)


DEFAULT_POLICY = {
    "metrics": {
        "rss_growth_mb": {"warn_max": 64.0, "fail_max": 128.0},
        "rss_slope_mb_per_hour": {"warn_max": 32.0, "fail_max": 64.0},
        "tps_drift_pct": {"warn_min": -5.0, "fail_min": -10.0},
        "bps_drift_pct": {"warn_min": -5.0, "fail_min": -10.0},
        "success_rate_end_pct": {"warn_min": 99.5, "fail_min": 99.0},
    }
}


def parse_float(value: str) -> Optional[float]:
    text = clean_text(value)
    if text == "" or text == "-1":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def load_realtime_samples(path: str) -> List[Dict[str, Optional[float]]]:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        rows = []
        for raw in reader:
            rows.append({key: parse_float(value) for key, value in raw.items()})
    return rows


def fit_slope(x_values: List[float], y_values: List[float]) -> Optional[float]:
    if len(x_values) < 2 or len(x_values) != len(y_values):
        return None
    x_mean = mean(x_values)
    y_mean = mean(y_values)
    numerator = sum((x - x_mean) * (y - y_mean) for x, y in zip(x_values, y_values))
    denominator = sum((x - x_mean) ** 2 for x in x_values)
    if denominator == 0:
        return None
    return numerator / denominator


def window_mean(values: List[float]) -> Optional[float]:
    if not values:
        return None
    return mean(values)


def format_num(value: Optional[float]) -> str:
    if value is None:
        return "N/A"
    return f"{value:.4f}"


def first_last(values: List[float]) -> (Optional[float], Optional[float]):
    if not values:
        return None, None
    return values[0], values[-1]


def compute_drift(start: Optional[float], end: Optional[float]) -> Optional[float]:
    if start is None or end is None:
        return None
    if start == 0:
        return 0.0 if end == 0 else None
    return ((end - start) / abs(start)) * 100.0


def compare_threshold(metric_value: Optional[float], spec: Dict[str, float]) -> str:
    if metric_value is None:
        return "SKIP"
    warn_min = spec.get("warn_min")
    fail_min = spec.get("fail_min")
    warn_max = spec.get("warn_max")
    fail_max = spec.get("fail_max")
    if fail_min is not None and metric_value < fail_min:
        return "FAIL"
    if fail_max is not None and metric_value > fail_max:
        return "FAIL"
    if warn_min is not None and metric_value < warn_min:
        return "WARN"
    if warn_max is not None and metric_value > warn_max:
        return "WARN"
    return "PASS"


def build_summary(samples: List[Dict[str, Optional[float]]], archive_row: Dict[str, str]) -> Dict[str, object]:
    valid_samples = [row for row in samples if row.get("RunTime(s)") is not None]
    summary: Dict[str, object] = {
        "scenario_id": archive_row.get("scenario_id") or "unknown",
        "sample_count": len(valid_samples),
        "analysis_window_s": valid_samples[-1]["RunTime(s)"] if valid_samples else 0.0,
        "longrun_status": "INSUFFICIENT_DATA",
        "suspicions": [],
        "rss_start_mb": None,
        "rss_end_mb": None,
        "rss_growth_mb": None,
        "rss_growth_pct": None,
        "rss_slope_mb_per_hour": None,
        "cpu_start_pct": None,
        "cpu_end_pct": None,
        "cpu_drift_pct": None,
        "single_core_cpu_start_pct": None,
        "single_core_cpu_end_pct": None,
        "single_core_cpu_drift_pct": None,
        "tps_start": None,
        "tps_end": None,
        "tps_drift_pct": None,
        "bps_start": None,
        "bps_end": None,
        "bps_drift_pct": None,
        "success_rate_min_pct": None,
        "success_rate_end_pct": None,
    }
    if len(valid_samples) < 3:
        return summary

    rss_points = [(row["RunTime(s)"], row["RSS(MB)"]) for row in valid_samples if row.get("RSS(MB)") is not None]
    cpu_values = [row["CPU(%)"] for row in valid_samples if row.get("CPU(%)") is not None]
    single_core_cpu_values = [row["SingleCoreCPU(%)"] for row in valid_samples if row.get("SingleCoreCPU(%)") is not None]
    tps_values = [row["Interval_TPS"] for row in valid_samples if row.get("Interval_TPS") is not None]
    bps_values = [row["Interval_BPS(Bytes/s)"] for row in valid_samples if row.get("Interval_BPS(Bytes/s)") is not None]
    success_values = [row["Success_Rate(%)"] for row in valid_samples if row.get("Success_Rate(%)") is not None]

    window_size = max(1, len(valid_samples) // 5)
    front = valid_samples[:window_size]
    back = valid_samples[-window_size:]

    rss_values = [value for _, value in rss_points]
    rss_times = [time for time, _ in rss_points]
    rss_start, rss_end = first_last(rss_values)
    cpu_start, cpu_end = first_last(cpu_values)
    single_core_cpu_start, single_core_cpu_end = first_last(single_core_cpu_values)
    tps_start = window_mean([row["Interval_TPS"] for row in front if row.get("Interval_TPS") is not None])
    tps_end = window_mean([row["Interval_TPS"] for row in back if row.get("Interval_TPS") is not None])
    bps_start = window_mean([row["Interval_BPS(Bytes/s)"] for row in front if row.get("Interval_BPS(Bytes/s)") is not None])
    bps_end = window_mean([row["Interval_BPS(Bytes/s)"] for row in back if row.get("Interval_BPS(Bytes/s)") is not None])
    success_end = window_mean([row["Success_Rate(%)"] for row in back if row.get("Success_Rate(%)") is not None])

    rss_growth_mb = None if rss_start is None or rss_end is None else rss_end - rss_start
    rss_growth_pct = compute_drift(rss_start, rss_end)
    rss_slope = fit_slope(rss_times, rss_values) if len(rss_points) >= 2 else None
    rss_slope_mb_per_hour = None if rss_slope is None else rss_slope * 3600.0

    summary.update(
        {
            "rss_start_mb": rss_start,
            "rss_end_mb": rss_end,
            "rss_growth_mb": rss_growth_mb,
            "rss_growth_pct": rss_growth_pct,
            "rss_slope_mb_per_hour": rss_slope_mb_per_hour,
            "cpu_start_pct": cpu_start,
            "cpu_end_pct": cpu_end,
            "cpu_drift_pct": compute_drift(cpu_start, cpu_end),
            "single_core_cpu_start_pct": single_core_cpu_start,
            "single_core_cpu_end_pct": single_core_cpu_end,
            "single_core_cpu_drift_pct": compute_drift(single_core_cpu_start, single_core_cpu_end),
            "tps_start": tps_start,
            "tps_end": tps_end,
            "tps_drift_pct": compute_drift(tps_start, tps_end),
            "bps_start": bps_start,
            "bps_end": bps_end,
            "bps_drift_pct": compute_drift(bps_start, bps_end),
            "success_rate_min_pct": min(success_values) if success_values else None,
            "success_rate_end_pct": success_end,
            "longrun_status": "PASS",
            "suspicions": [],
        }
    )
    return summary


def apply_policy(summary: Dict[str, object], policy: Dict[str, object]) -> Dict[str, object]:
    metrics = (policy or {}).get("metrics", {})
    results = []
    overall = "PASS"
    suspicions = list(summary.get("suspicions", []))

    mapping = {
        "rss_growth_mb": "memory_growth_detected",
        "rss_slope_mb_per_hour": "memory_leak_suspected",
        "tps_drift_pct": "throughput_drift_detected",
        "bps_drift_pct": "bandwidth_drift_detected",
        "success_rate_end_pct": "success_rate_drift_detected",
    }

    for metric_name, spec in metrics.items():
        metric_value = summary.get(metric_name)
        if isinstance(metric_value, (int, float)):
            metric_value = float(metric_value)
        else:
            metric_value = None
        status = compare_threshold(metric_value, spec or {})
        results.append(
            {
                "metric": metric_name,
                "value": metric_value,
                "status": status,
                "spec": spec,
            }
        )
        if status == "FAIL":
            overall = "FAIL"
            suspicions.append(mapping.get(metric_name, metric_name))
        elif status == "WARN" and overall != "FAIL":
            overall = "WARN"
            suspicions.append(mapping.get(metric_name, metric_name))

    if summary.get("longrun_status") == "INSUFFICIENT_DATA":
        overall = "INSUFFICIENT_DATA"

    summary["policy_results"] = results
    summary["longrun_status"] = overall
    summary["suspicions"] = sorted(set(suspicions))
    return summary


def write_markdown(path: str, summary: Dict[str, object]) -> None:
    label_map = {
        "rss_start_mb": "rss_start",
        "rss_end_mb": "rss_end",
        "rss_growth_mb": "rss_growth",
        "rss_growth_pct": "rss_growth_pct",
        "rss_slope_mb_per_hour": "rss_slope_mb_per_hour",
        "cpu_start_pct": "cpu_start_pct",
        "cpu_end_pct": "cpu_end_pct",
        "cpu_drift_pct": "cpu_drift_pct",
        "single_core_cpu_start_pct": "single_core_cpu_start_pct",
        "single_core_cpu_end_pct": "single_core_cpu_end_pct",
        "single_core_cpu_drift_pct": "single_core_cpu_drift_pct",
        "tps_start": "tps_start",
        "tps_end": "tps_end",
        "tps_drift_pct": "tps_drift_pct",
        "bps_start": "bps_start",
        "bps_end": "bps_end",
        "bps_drift_pct": "bps_drift_pct",
        "success_rate_min_pct": "success_rate_min_pct",
        "success_rate_end_pct": "success_rate_end_pct",
    }
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("# Long-Run Analysis\n\n")
        handle.write(f"- Scenario: `{summary.get('scenario_id', 'unknown')}`\n")
        handle.write(f"- Status: `{summary.get('longrun_status', 'UNKNOWN')}`\n")
        handle.write(f"- Samples: `{summary.get('sample_count', 0)}`\n")
        window_s = summary.get("analysis_window_s")
        handle.write(f"- Window: `{humanize_duration_s(window_s) if isinstance(window_s, (int, float)) else 'N/A'}`")
        if isinstance(window_s, (int, float)):
            handle.write(f" (`{format_num(window_s)} s`)")
        handle.write("\n\n")
        handle.write("## Summary\n\n")
        handle.write("| Metric | Value |\n")
        handle.write("| --- | --- |\n")
        for key in [
            "rss_start_mb",
            "rss_end_mb",
            "rss_growth_mb",
            "rss_growth_pct",
            "rss_slope_mb_per_hour",
            "cpu_start_pct",
            "cpu_end_pct",
            "cpu_drift_pct",
            "single_core_cpu_start_pct",
            "single_core_cpu_end_pct",
            "single_core_cpu_drift_pct",
            "tps_start",
            "tps_end",
            "tps_drift_pct",
            "bps_start",
            "bps_end",
            "bps_drift_pct",
            "success_rate_min_pct",
            "success_rate_end_pct",
        ]:
            value = summary.get(key)
            if key.startswith("rss_") and key.endswith("_mb"):
                rendered = format_human_metric("avg_rss_mb", value) if isinstance(value, (int, float)) else "N/A (MB)"
            elif key in ("bps_start", "bps_end"):
                rendered = format_human_metric("final_bps_bytes_per_sec", value) if isinstance(value, (int, float)) else "N/A (Bytes/s)"
            elif key.endswith("_pct"):
                rendered = f"{float(value):.2f}%" if isinstance(value, (int, float)) else "N/A (%)"
            else:
                rendered = format_num(value if isinstance(value, (int, float)) else None)
            handle.write(f"| `{label_map.get(key, key)}` | `{rendered}` |\n")
        handle.write("\n## Suspicions\n\n")
        suspicions = summary.get("suspicions") or []
        if suspicions:
            for item in suspicions:
                handle.write(f"- `{item}`\n")
        else:
            handle.write("- None\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze long-run benchmark trends from realtime.txt and archive.csv.")
    parser.add_argument("--realtime", required=True, help="Path to realtime.txt")
    parser.add_argument("--archive", required=True, help="Path to archive.csv")
    parser.add_argument("--output-dir", required=True, help="Directory for longrun_summary outputs")
    parser.add_argument("--policy", help="Optional YAML/JSON policy file")
    parser.add_argument("--gate", action="store_true", help="Return non-zero when longrun_status is FAIL")
    args = parser.parse_args()

    try:
        ensure_directory(args.output_dir)
        archive_row = load_archive_row(args.archive)
        samples = load_realtime_samples(args.realtime)
        summary = build_summary(samples, archive_row)
        policy = DEFAULT_POLICY
        if args.policy:
            loaded = load_policy_file(args.policy) or {}
            policy = loaded if isinstance(loaded, dict) else DEFAULT_POLICY
        summary = apply_policy(summary, policy)

        json_path = os.path.join(args.output_dir, "longrun_summary.json")
        md_path = os.path.join(args.output_dir, "longrun_summary.md")
        with open(json_path, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=2, ensure_ascii=False)
        write_markdown(md_path, summary)

        print(f"[+] Scenario: {summary.get('scenario_id')}")
        print(f"[+] longrun_summary.json -> {json_path}")
        print(f"[+] longrun_summary.md   -> {md_path}")
        print(f"[+] Long-run status: {summary.get('longrun_status')}")

        if args.gate and summary.get("longrun_status") == "FAIL":
            return 1
        if summary.get("longrun_status") == "INSUFFICIENT_DATA":
            return 2 if args.gate else 0
        return 0
    except Exception as exc:
        print(f"[-] Failed to analyze long-run results: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
