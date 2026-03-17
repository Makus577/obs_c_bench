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

        if row["status"] == "SKIP":
            gate_row["gate_status"] = "SKIP"
            skipped_metrics.append(metric_name)
            rows.append(gate_row)
            continue

        diff_pct = row["relative_diff_pct"]
        direction = spec.get("direction", row["direction"])
        warn_threshold = float(spec.get("warn_threshold_pct", 0.0))
        fail_threshold = float(spec.get("fail_threshold_pct", 0.0))

        if diff_pct is None:
            gate_row["gate_status"] = "SKIP"
            gate_row["reason"] = row["reason"] or "Percentage diff unavailable."
            skipped_metrics.append(metric_name)
            rows.append(gate_row)
            continue

        regression_pct = 0.0
        if direction == "higher":
            if diff_pct < 0:
                regression_pct = -diff_pct
        else:
            if diff_pct > 0:
                regression_pct = diff_pct

        if regression_pct >= fail_threshold:
            gate_row["gate_status"] = "FAIL"
            failed_metrics.append(metric_name)
        elif regression_pct >= warn_threshold:
            gate_row["gate_status"] = "WARN"
            warned_metrics.append(metric_name)
        else:
            gate_row["gate_status"] = "PASS"

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
        elif args.baselines_manifest and scenario_id:
            baseline_path, baseline_ref = resolve_baseline_from_manifest(args.baselines_manifest, scenario_id)
        else:
            raise ValueError("Either --baseline or (--baselines-manifest and --scenario-id) must be provided.")

        baseline_row = load_archive_row(baseline_path)
        policy = load_policy_file(args.policy)
        metric_specs = merge_metric_specs(policy)
        compare_result = compare_archives(
            baseline_row,
            candidate_row,
            metric_specs=metric_specs,
            required_fields=policy.get("required_match_fields"),
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
