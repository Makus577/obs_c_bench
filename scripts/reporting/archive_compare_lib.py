#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import csv
import json
import os
import re


DEFAULT_REQUIRED_MATCH_FIELDS = [
    "op",
    "total_threads",
    "object_size_spec",
    "users_loaded",
]

DEFAULT_ADVISORY_MATCH_FIELDS = [
    "config_file",
    "users_file",
    "host_os",
    "host_arch",
]

DEFAULT_METRIC_SPECS = {
    "final_tps": {
        "label": "Final TPS",
        "direction": "higher",
        "enabled": True,
        "warn_threshold_pct": 3.0,
        "fail_threshold_pct": 5.0,
    },
    "final_bps_bytes_per_sec": {
        "label": "Final BPS",
        "direction": "higher",
        "enabled": True,
        "warn_threshold_pct": 3.0,
        "fail_threshold_pct": 5.0,
    },
    "avg_latency_ms": {
        "label": "Avg Latency",
        "direction": "lower",
        "enabled": True,
        "warn_threshold_pct": 5.0,
        "fail_threshold_pct": 8.0,
    },
    "p99_latency_ms": {
        "label": "P99 Latency",
        "direction": "lower",
        "enabled": True,
        "warn_threshold_pct": 7.0,
        "fail_threshold_pct": 10.0,
    },
    "avg_cpu_pct": {
        "label": "Avg CPU",
        "direction": "lower",
        "enabled": True,
        "warn_threshold_pct": 7.0,
        "fail_threshold_pct": 10.0,
    },
    "success_rate_pct": {
        "label": "Success Rate",
        "direction": "higher",
        "enabled": True,
        "warn_threshold_pct": 0.5,
        "fail_threshold_pct": 1.0,
        "absolute_warn_min": 99.9,
        "absolute_fail_min": 99.0,
    },
    "failed_requests": {
        "label": "Failed Requests",
        "direction": "lower",
        "enabled": True,
        "warn_threshold_pct": 10.0,
        "fail_threshold_pct": 20.0,
        "absolute_warn_max": 0.0,
        "absolute_fail_max": 0.0,
    },
    "peak_tps": {
        "label": "Peak TPS",
        "direction": "higher",
        "enabled": True,
        "warn_threshold_pct": 5.0,
        "fail_threshold_pct": 8.0,
    },
    "peak_bps_bytes_per_sec": {
        "label": "Peak BPS",
        "direction": "higher",
        "enabled": True,
        "warn_threshold_pct": 5.0,
        "fail_threshold_pct": 8.0,
    },
    "peak_cpu_pct": {
        "label": "Peak CPU",
        "direction": "lower",
        "enabled": True,
        "warn_threshold_pct": 8.0,
        "fail_threshold_pct": 12.0,
    },
    "avg_rss_mb": {
        "label": "Avg RSS",
        "direction": "lower",
        "enabled": True,
        "warn_threshold_pct": 8.0,
        "fail_threshold_pct": 12.0,
    },
    "peak_rss_mb": {
        "label": "Peak RSS",
        "direction": "lower",
        "enabled": True,
        "warn_threshold_pct": 8.0,
        "fail_threshold_pct": 12.0,
    },
    "avg_single_stream_bps": {
        "label": "Avg Single Stream",
        "direction": "higher",
        "enabled": True,
        "warn_threshold_pct": 4.0,
        "fail_threshold_pct": 6.0,
    },
    "max_single_stream_bps": {
        "label": "Max Single Stream",
        "direction": "higher",
        "enabled": True,
        "warn_threshold_pct": 5.0,
        "fail_threshold_pct": 8.0,
    },
}


def clean_text(value):
    if value is None:
        return ""
    return str(value).strip()


def slugify(value):
    text = clean_text(value).lower()
    text = text.replace("~", "_to_")
    text = re.sub(r"[^a-z0-9]+", "_", text)
    return text.strip("_") or "unknown"


def infer_scenario_id(row):
    return f"{slugify(row.get('op'))}_{slugify(row.get('object_size_spec'))}_{slugify(row.get('total_threads'))}t"


def load_archive_row(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"Archive file not found: {path}")

    with open(path, "r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        row = next(reader, None)
        if row is None:
            raise ValueError(f"Archive file is empty: {path}")

    normalized = {key: clean_text(value) for key, value in row.items()}
    normalized["__source_path"] = os.path.abspath(path)
    if not normalized.get("scenario_id"):
        normalized["scenario_id"] = infer_scenario_id(normalized)
    return normalized


def parse_metric_value(raw_value):
    text = clean_text(raw_value)
    if text == "" or text == "-1":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def format_metric_value(value):
    if value is None:
        return "N/A"
    if abs(value) >= 1000:
        return f"{value:.4f}"
    return f"{value:.4f}"


def compute_relative_diff_pct(baseline_value, candidate_value):
    if baseline_value == 0:
        return 0.0 if candidate_value == 0 else None
    return ((candidate_value - baseline_value) / abs(baseline_value)) * 100.0


def compare_metric(metric_name, spec, baseline_row, candidate_row):
    baseline_value = parse_metric_value(baseline_row.get(metric_name))
    candidate_value = parse_metric_value(candidate_row.get(metric_name))
    result = {
        "metric": metric_name,
        "label": spec.get("label", metric_name),
        "direction": spec.get("direction", "higher"),
        "baseline_value": baseline_value,
        "candidate_value": candidate_value,
        "absolute_diff": None,
        "relative_diff_pct": None,
        "trend": "SKIP",
        "status": "SKIP",
        "reason": "",
        "gate_status": "",
    }

    if baseline_value is None:
        result["reason"] = "Baseline value missing or invalid."
        return result
    if candidate_value is None:
        result["reason"] = "Candidate value missing or invalid."
        return result

    result["absolute_diff"] = candidate_value - baseline_value
    result["relative_diff_pct"] = compute_relative_diff_pct(baseline_value, candidate_value)

    if candidate_value == baseline_value:
        result["trend"] = "UNCHANGED"
    elif spec.get("direction") == "higher":
        result["trend"] = "IMPROVED" if candidate_value > baseline_value else "REGRESSED"
    else:
        result["trend"] = "IMPROVED" if candidate_value < baseline_value else "REGRESSED"

    if result["relative_diff_pct"] is None:
        result["status"] = "SKIP"
        result["reason"] = "Baseline value is zero; percentage diff is undefined."
    else:
        result["status"] = "OK"

    return result


def validate_comparable(baseline_row, candidate_row, required_fields=None):
    mismatches = []
    fields = required_fields or DEFAULT_REQUIRED_MATCH_FIELDS
    for field in fields:
        baseline_value = clean_text(baseline_row.get(field))
        candidate_value = clean_text(candidate_row.get(field))
        if baseline_value != candidate_value:
            mismatches.append(
                {
                    "field": field,
                    "baseline_value": baseline_value,
                    "candidate_value": candidate_value,
                }
            )
    return mismatches


def validate_advisory_fields(baseline_row, candidate_row, advisory_fields=None):
    mismatches = []
    fields = advisory_fields or DEFAULT_ADVISORY_MATCH_FIELDS
    for field in fields:
        baseline_value = clean_text(baseline_row.get(field))
        candidate_value = clean_text(candidate_row.get(field))
        if baseline_value and candidate_value and baseline_value != candidate_value:
            mismatches.append(
                {
                    "field": field,
                    "baseline_value": baseline_value,
                    "candidate_value": candidate_value,
                }
            )
    return mismatches


def compare_archives(baseline_row, candidate_row, metric_specs=None, required_fields=None, advisory_fields=None):
    specs = metric_specs or DEFAULT_METRIC_SPECS
    mismatches = validate_comparable(baseline_row, candidate_row, required_fields)
    advisory_mismatches = validate_advisory_fields(baseline_row, candidate_row, advisory_fields)
    rows = [compare_metric(metric_name, spec, baseline_row, candidate_row) for metric_name, spec in specs.items()]
    return {
        "baseline": baseline_row,
        "candidate": candidate_row,
        "mismatches": mismatches,
        "advisory_mismatches": advisory_mismatches,
        "rows": rows,
        "required_match_fields": required_fields or DEFAULT_REQUIRED_MATCH_FIELDS,
        "advisory_match_fields": advisory_fields or DEFAULT_ADVISORY_MATCH_FIELDS,
    }


def ensure_directory(path):
    os.makedirs(path, exist_ok=True)


def resolve_output_dir(output_dir, candidate_archive_path):
    if output_dir:
        ensure_directory(output_dir)
        return os.path.abspath(output_dir)
    default_dir = os.path.dirname(os.path.abspath(candidate_archive_path))
    ensure_directory(default_dir)
    return default_dir


def write_compare_csv(rows, output_path):
    fieldnames = [
        "metric",
        "label",
        "direction",
        "baseline_value",
        "candidate_value",
        "absolute_diff",
        "relative_diff_pct",
        "trend",
        "status",
        "gate_status",
        "reason",
    ]
    with open(output_path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def render_meta_table(row):
    keys = [
        "scenario_id",
        "op",
        "total_threads",
        "object_size_spec",
        "users_loaded",
        "config_file",
        "users_file",
        "git_commit",
        "host_os",
        "host_arch",
        "__source_path",
    ]
    lines = ["| Field | Value |", "| --- | --- |"]
    for key in keys:
        value = clean_text(row.get(key)) or "N/A"
        lines.append(f"| `{key}` | `{value}` |")
    return "\n".join(lines)


def render_metric_table(rows):
    lines = [
        "| Metric | Direction | Baseline | Candidate | Abs Diff | Diff % | Trend | Status | Reason |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        abs_diff = "N/A" if row["absolute_diff"] is None else f"{row['absolute_diff']:.4f}"
        rel_diff = "N/A" if row["relative_diff_pct"] is None else f"{row['relative_diff_pct']:.2f}%"
        status = row.get("gate_status") or row["status"]
        reason = row["reason"] or ""
        lines.append(
            f"| {row['label']} | {row['direction']} | {format_metric_value(row['baseline_value'])} | "
            f"{format_metric_value(row['candidate_value'])} | {abs_diff} | {rel_diff} | "
            f"{row['trend']} | {status} | {reason} |"
        )
    return "\n".join(lines)


def write_compare_md(compare_result, output_path, title):
    with open(output_path, "w", encoding="utf-8") as handle:
        handle.write(f"# {title}\n\n")
        if compare_result["mismatches"]:
            handle.write("## Comparability Check Failed\n\n")
            handle.write("| Field | Baseline | Candidate |\n")
            handle.write("| --- | --- | --- |\n")
            for mismatch in compare_result["mismatches"]:
                handle.write(
                    f"| `{mismatch['field']}` | `{mismatch['baseline_value'] or 'N/A'}` | "
                    f"`{mismatch['candidate_value'] or 'N/A'}` |\n"
                )
            handle.write("\n")

        if compare_result.get("advisory_mismatches"):
            handle.write("## Advisory Field Differences\n\n")
            handle.write("| Field | Baseline | Candidate |\n")
            handle.write("| --- | --- | --- |\n")
            for mismatch in compare_result["advisory_mismatches"]:
                handle.write(
                    f"| `{mismatch['field']}` | `{mismatch['baseline_value'] or 'N/A'}` | "
                    f"`{mismatch['candidate_value'] or 'N/A'}` |\n"
                )
            handle.write("\n")

        handle.write("## Baseline\n\n")
        handle.write(render_meta_table(compare_result["baseline"]))
        handle.write("\n\n## Candidate\n\n")
        handle.write(render_meta_table(compare_result["candidate"]))
        handle.write("\n\n## Metrics\n\n")
        handle.write(render_metric_table(compare_result["rows"]))
        handle.write("\n")


def load_policy_file(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"Policy file not found: {path}")

    if path.endswith(".json"):
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)

    if path.endswith((".yaml", ".yml")):
        try:
            import yaml  # type: ignore
        except ImportError as exc:
            raise RuntimeError("YAML policy requires PyYAML. Use JSON or install PyYAML.") from exc
        with open(path, "r", encoding="utf-8") as handle:
            return yaml.safe_load(handle)

    raise ValueError(f"Unsupported policy file format: {path}")


def merge_metric_specs(policy):
    merged = {}
    policy_metrics = policy.get("metrics", {})
    for metric_name, default_spec in DEFAULT_METRIC_SPECS.items():
        merged_spec = dict(default_spec)
        merged_spec.update(policy_metrics.get(metric_name, {}))
        merged[metric_name] = merged_spec
    for metric_name, policy_spec in policy_metrics.items():
        if metric_name not in merged:
            merged[metric_name] = dict(policy_spec)
    return merged


def _row_matches_manifest_fields(candidate_row, manifest_row):
    checks = [
        ("scenario_id", clean_text(manifest_row.get("scenario_id"))),
        ("op", clean_text(manifest_row.get("op"))),
        ("object_size_spec", clean_text(manifest_row.get("object_size_spec"))),
        ("users_loaded", clean_text(manifest_row.get("users_loaded"))),
    ]
    manifest_threads = clean_text(manifest_row.get("threads"))
    if manifest_threads:
        checks.append(("total_threads", manifest_threads))

    for field, expected in checks:
        if not expected:
            continue
        actual = clean_text(candidate_row.get(field))
        if actual != expected:
            return False
    return True


def resolve_baseline_from_manifest(manifest_path, scenario_id=None, candidate_row=None):
    manifest_dir = os.path.dirname(os.path.abspath(manifest_path))
    if not os.path.exists(manifest_path):
        raise FileNotFoundError(f"Baseline manifest not found: {manifest_path}")

    if manifest_path.endswith(".json"):
        with open(manifest_path, "r", encoding="utf-8") as handle:
            rows = json.load(handle)
    else:
        with open(manifest_path, "r", encoding="utf-8", newline="") as handle:
            rows = list(csv.DictReader(handle))

    matched_row = None
    for row in rows:
        if scenario_id and clean_text(row.get("scenario_id")) == scenario_id:
            matched_row = row
            break
        if candidate_row and _row_matches_manifest_fields(candidate_row, row):
            matched_row = row
            break

    if matched_row is None:
        if scenario_id:
            raise KeyError(f"Scenario '{scenario_id}' not found in baseline manifest: {manifest_path}")
        raise KeyError("No matching baseline entry found in baseline manifest for the candidate archive.")

    row = matched_row
    baseline_path = clean_text(row.get("baseline_archive_path"))
    if baseline_path:
        if not os.path.isabs(baseline_path):
            baseline_path = os.path.abspath(os.path.join(manifest_dir, baseline_path))
        return baseline_path, row
    baseline_url = clean_text(row.get("baseline_archive_url"))
    if baseline_url:
        raise RuntimeError("Remote baseline URLs are not supported in v1. Use baseline_archive_path.")
    raise KeyError("Baseline entry found, but no usable baseline_archive_path was provided.")


def summarize_rows(rows, status_key):
    summary = {"PASS": 0, "WARN": 0, "FAIL": 0, "SKIP": 0, "OK": 0}
    for row in rows:
        summary[row.get(status_key, "SKIP")] = summary.get(row.get(status_key, "SKIP"), 0) + 1
    return summary
