#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import os
import sys

from archive_compare_lib import (
    compare_archives,
    ensure_directory,
    load_archive_row,
    load_policy_file,
    merge_metric_specs,
    resolve_baseline_from_manifest,
    resolve_output_dir,
    summarize_rows,
    write_compare_csv,
    write_compare_md,
)


def evaluate_gate(compare_result, metric_specs):
    rows = []
    failed_metrics = []
    warned_metrics = []
    skipped_metrics = []

    for row in compare_result["rows"]:
        metric_name = row["metric"]
        spec = metric_specs.get(metric_name, {})
        enabled = bool(spec.get("enabled", False))
        gate_row = dict(row)

        if not enabled:
            gate_row["gate_status"] = "INFO"
            rows.append(gate_row)
            continue

        candidate_value = row["candidate_value"]
        diff_pct = row["relative_diff_pct"]
        direction = spec.get("direction", row["direction"])
        warn_threshold = float(spec.get("warn_threshold_pct", 0.0))
        fail_threshold = float(spec.get("fail_threshold_pct", 0.0))
        absolute_warn_min = spec.get("absolute_warn_min")
        absolute_fail_min = spec.get("absolute_fail_min")
        absolute_warn_max = spec.get("absolute_warn_max")
        absolute_fail_max = spec.get("absolute_fail_max")
        has_absolute_guard = any(
            value is not None
            for value in (absolute_warn_min, absolute_fail_min, absolute_warn_max, absolute_fail_max)
        )

        if row["status"] == "SKIP" and not has_absolute_guard:
            gate_row["gate_status"] = "SKIP"
            skipped_metrics.append(metric_name)
            rows.append(gate_row)
            continue

        if diff_pct is None and not has_absolute_guard:
            gate_row["gate_status"] = "SKIP"
            gate_row["reason"] = row["reason"] or "Percentage diff unavailable."
            skipped_metrics.append(metric_name)
            rows.append(gate_row)
            continue

        regression_pct = 0.0
        if diff_pct is not None and direction == "higher":
            if diff_pct < 0:
                regression_pct = -diff_pct
        elif diff_pct is not None:
            if diff_pct > 0:
                regression_pct = diff_pct

        gate_status = "PASS"
        gate_reason = ""

        if absolute_fail_min is not None and candidate_value is not None and candidate_value < float(absolute_fail_min):
            gate_status = "FAIL"
            gate_reason = f"Candidate value {candidate_value:.4f} is below absolute fail minimum {float(absolute_fail_min):.4f}."
        elif absolute_fail_max is not None and candidate_value is not None and candidate_value > float(absolute_fail_max):
            gate_status = "FAIL"
            gate_reason = f"Candidate value {candidate_value:.4f} is above absolute fail maximum {float(absolute_fail_max):.4f}."
        elif diff_pct is not None and regression_pct >= fail_threshold:
            gate_status = "FAIL"
        elif absolute_warn_min is not None and candidate_value is not None and candidate_value < float(absolute_warn_min):
            gate_status = "WARN"
            gate_reason = f"Candidate value {candidate_value:.4f} is below absolute warn minimum {float(absolute_warn_min):.4f}."
        elif absolute_warn_max is not None and candidate_value is not None and candidate_value > float(absolute_warn_max):
            gate_status = "WARN"
            gate_reason = f"Candidate value {candidate_value:.4f} is above absolute warn maximum {float(absolute_warn_max):.4f}."
        elif diff_pct is not None and regression_pct >= warn_threshold:
            gate_status = "WARN"

        gate_row["gate_status"] = gate_status
        if gate_reason:
            gate_row["reason"] = gate_reason

        if gate_status == "FAIL":
            gate_row["gate_status"] = "FAIL"
            failed_metrics.append(metric_name)
        elif gate_status == "WARN":
            warned_metrics.append(metric_name)

        rows.append(gate_row)

    overall_status = "FAIL" if failed_metrics else "PASS"
    return {
        "rows": rows,
        "overall_status": overall_status,
        "failed_metrics": failed_metrics,
        "warned_metrics": warned_metrics,
        "skipped_metrics": skipped_metrics,
    }


def main():
    parser = argparse.ArgumentParser(description="Run conservative performance regression gate based on archive.csv files.")
    parser.add_argument("--baseline", help="Path to baseline archive.csv")
    parser.add_argument("--candidate", required=True, help="Path to candidate archive.csv")
    parser.add_argument("--policy", required=True, help="Path to gate policy JSON/YAML file")
    parser.add_argument("--baselines-manifest", help="Path to baseline manifest CSV/JSON")
    parser.add_argument("--scenario-id", help="Scenario id used to resolve baseline from the manifest")
    parser.add_argument("--output-dir", default=None, help="Directory for compare/gate output files")
    args = parser.parse_args()

    try:
        candidate_row = load_archive_row(args.candidate)
        scenario_id = args.scenario_id or candidate_row.get("scenario_id")

        if args.baseline:
            baseline_path = args.baseline
            baseline_ref = None
        elif args.baselines_manifest:
            baseline_path, baseline_ref = resolve_baseline_from_manifest(
                args.baselines_manifest,
                scenario_id=scenario_id,
                candidate_row=candidate_row,
            )
        else:
            raise ValueError("Either --baseline or --baselines-manifest must be provided.")

        baseline_row = load_archive_row(baseline_path)
        policy = load_policy_file(args.policy)
        metric_specs = merge_metric_specs(policy)
        compare_result = compare_archives(
            baseline_row,
            candidate_row,
            metric_specs=metric_specs,
            required_fields=policy.get("required_match_fields"),
            advisory_fields=policy.get("advisory_match_fields"),
        )

        output_dir = resolve_output_dir(args.output_dir, args.candidate)
        ensure_directory(output_dir)

        if compare_result["mismatches"]:
            gate_result = {
                "overall_status": "INCOMPARABLE",
                "exit_code": 2,
                "scenario_id": scenario_id,
                "baseline": baseline_row,
                "candidate": candidate_row,
                "baseline_ref": baseline_ref,
                "mismatches": compare_result["mismatches"],
                "advisory_mismatches": compare_result.get("advisory_mismatches", []),
                "rows": compare_result["rows"],
            }
        else:
            evaluation = evaluate_gate(compare_result, metric_specs)
            compare_result["rows"] = evaluation["rows"]
            gate_result = {
                "overall_status": evaluation["overall_status"],
                "exit_code": 1 if evaluation["failed_metrics"] else 0,
                "scenario_id": scenario_id,
                "baseline": baseline_row,
                "candidate": candidate_row,
                "baseline_ref": baseline_ref,
                "failed_metrics": evaluation["failed_metrics"],
                "warned_metrics": evaluation["warned_metrics"],
                "skipped_metrics": evaluation["skipped_metrics"],
                "advisory_mismatches": compare_result.get("advisory_mismatches", []),
                "rows": evaluation["rows"],
            }

        compare_csv = os.path.join(output_dir, "compare.csv")
        compare_md = os.path.join(output_dir, "compare.md")
        gate_json = os.path.join(output_dir, "gate_result.json")
        write_compare_csv(compare_result["rows"], compare_csv)
        write_compare_md(compare_result, compare_md, "Performance Gate Comparison Report")
        with open(gate_json, "w", encoding="utf-8") as handle:
            json.dump(gate_result, handle, indent=2, ensure_ascii=False)

        print(f"[+] Scenario:   {scenario_id}")
        print(f"[+] Baseline:   {baseline_row['__source_path']}")
        print(f"[+] Candidate:  {candidate_row['__source_path']}")
        print(f"[+] compare.csv -> {compare_csv}")
        print(f"[+] compare.md  -> {compare_md}")
        print(f"[+] gate_result -> {gate_json}")
        if gate_result.get("advisory_mismatches"):
            print("[*] Advisory differences detected:")
            for mismatch in gate_result["advisory_mismatches"]:
                print(
                    f"    {mismatch['field']}: baseline={mismatch['baseline_value'] or 'N/A'} "
                    f"candidate={mismatch['candidate_value'] or 'N/A'}"
                )

        if gate_result["exit_code"] == 2:
            print("[-] Performance gate skipped: archives are not comparable.")
            for mismatch in compare_result["mismatches"]:
                print(
                    f"    {mismatch['field']}: baseline={mismatch['baseline_value'] or 'N/A'} "
                    f"candidate={mismatch['candidate_value'] or 'N/A'}"
                )
            return 2

        summary = summarize_rows(compare_result["rows"], "gate_status")
        print(
            f"[+] Gate summary: PASS={summary.get('PASS', 0)} "
            f"WARN={summary.get('WARN', 0)} FAIL={summary.get('FAIL', 0)} "
            f"SKIP={summary.get('SKIP', 0)} INFO={summary.get('INFO', 0)}"
        )
        if gate_result["exit_code"] == 1:
            print(f"[-] Gate failed on metrics: {', '.join(gate_result['failed_metrics'])}")
        else:
            print("[+] Gate passed.")
        return gate_result["exit_code"]
    except Exception as exc:
        print(f"[-] Failed to execute perf gate: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
