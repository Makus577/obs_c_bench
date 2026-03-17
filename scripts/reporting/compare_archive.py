#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import sys

from archive_compare_lib import (
    compare_archives,
    ensure_directory,
    load_archive_row,
    resolve_output_dir,
    summarize_rows,
    write_compare_csv,
    write_compare_md,
)


def main():
    parser = argparse.ArgumentParser(description="Compare two archive.csv files and generate human/machine readable reports.")
    parser.add_argument("--baseline", required=True, help="Path to baseline archive.csv")
    parser.add_argument("--candidate", required=True, help="Path to candidate archive.csv")
    parser.add_argument("--output-dir", default=None, help="Directory for compare.csv and compare.md")
    args = parser.parse_args()

    try:
        baseline_row = load_archive_row(args.baseline)
        candidate_row = load_archive_row(args.candidate)
        compare_result = compare_archives(baseline_row, candidate_row)
        output_dir = resolve_output_dir(args.output_dir, args.candidate)
        ensure_directory(output_dir)
        compare_csv = os.path.join(output_dir, "compare.csv")
        compare_md = os.path.join(output_dir, "compare.md")
        write_compare_csv(compare_result["rows"], compare_csv)
        write_compare_md(compare_result, compare_md, "Archive Comparison Report")

        print(f"[+] Baseline:  {baseline_row['__source_path']}")
        print(f"[+] Candidate: {candidate_row['__source_path']}")
        print(f"[+] Scenario:  {candidate_row.get('scenario_id', 'N/A')}")
        print(f"[+] compare.csv -> {compare_csv}")
        print(f"[+] compare.md  -> {compare_md}")

        if compare_result["mismatches"]:
            print("[-] Archive files are not comparable:")
            for mismatch in compare_result["mismatches"]:
                print(
                    f"    {mismatch['field']}: baseline={mismatch['baseline_value'] or 'N/A'} "
                    f"candidate={mismatch['candidate_value'] or 'N/A'}"
                )
            return 2

        summary = summarize_rows(compare_result["rows"], "status")
        print(
            f"[+] Metrics compared: OK={summary.get('OK', 0)} "
            f"SKIP={summary.get('SKIP', 0)}"
        )
        return 0
    except Exception as exc:
        print(f"[-] Failed to compare archives: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
