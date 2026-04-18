#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Benchmark orchestration engine: chains benchmark -> merge -> analyze -> gate -> plot."""

import glob
import json
import os
import re
import subprocess
import sys
from datetime import datetime
from typing import Any, Dict, List, Optional


BASE_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BINARY = os.path.join(BASE_DIR, "obs_c_bench")
REPO_ROOT = BASE_DIR
DEFAULT_ARCHIVE_NAME = "archive.csv"
DEFAULT_REALTIME_NAME = "realtime.txt"
DEFAULT_LONGRUN_POLICY = os.path.join(REPO_ROOT, "ci", "perf", "longrun_policy.yaml")
DEFAULT_PERF_POLICY = os.path.join(REPO_ROOT, "ci", "perf", "gate_policy.json")


def generate_run_id(identifier: Optional[str]) -> str:
    """Generate a run ID from identifier and timestamp."""
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    if not identifier:
        return f"run_{timestamp}"
    slug = re.sub(r'[^a-z0-9]+', '_', identifier.lower()).strip('_')
    return f"{slug}_{timestamp}"


def resolve_benchmark_binary(path: Optional[str]) -> str:
    """Resolve the benchmark binary path."""
    if path and os.path.isfile(path):
        return os.path.abspath(path)
    # Auto-detect: prefer obs_c_bench, fall back to obs_c_bench_mock
    for name in ("obs_c_bench", "obs_c_bench_mock"):
        candidate = os.path.join(BASE_DIR, name)
        if os.path.isfile(candidate):
            return candidate
    return DEFAULT_BINARY


def ensure_output_dirs(run_id: str, output_dir: str) -> Dict[str, str]:
    """Create output directory structure and return paths dict."""
    root = os.path.join(output_dir, run_id)
    return {
        "root": root,
        "benchmark": os.path.join(root, "benchmark"),
        "analysis": os.path.join(root, "analysis"),
        "gate": os.path.join(root, "gate"),
        "dashboard": os.path.join(root, "dashboard"),
    }


def find_latest_task_dir() -> Optional[str]:
    """Find the most recent logs/task_* directory."""
    task_dirs = sorted(glob.glob(os.path.join(BASE_DIR, "logs", "task_*")))
    return task_dirs[-1] if task_dirs else None


def run_single_scenario(
    config_path: str,
    output_subdir: str,
    binary: str,
    skip_plot: bool,
    scenario_id: str,
) -> Dict[str, Any]:
    """Execute one scenario and chain all downstream stages."""
    # 1. Run benchmark binary
    benchmark_result = subprocess.run(
        [binary, "--config", config_path],
        check=False,
        capture_output=True,
        text=True,
    )
    if benchmark_result.returncode != 0:
        print(f"[-] Benchmark failed with exit code {benchmark_result.returncode}")
        if benchmark_result.stdout:
            print(benchmark_result.stdout)
        if benchmark_result.stderr:
            print(benchmark_result.stderr)
        return {"exit_code": benchmark_result.returncode, "task_dir": None, "scenario_id": scenario_id}

    # 2. Find task directory
    task_dir = find_latest_task_dir()
    if not task_dir:
        print("[-] Could not find benchmark task log directory")
        return {"exit_code": 1, "task_dir": None, "scenario_id": scenario_id}

    archive_path = os.path.join(task_dir, DEFAULT_ARCHIVE_NAME)
    realtime_path = os.path.join(task_dir, DEFAULT_REALTIME_NAME)

    # 3. Run merge_details
    print("[+] Stage: merge_details")
    merge_result = subprocess.run(
        [sys.executable, "-m", "scripts.reporting.merge_details"],
        check=False,
        capture_output=True,
        text=True,
    )
    if merge_result.returncode != 0:
        print(f"[-] merge_details failed: {merge_result.stderr}")
        return {"exit_code": merge_result.returncode, "task_dir": task_dir, "scenario_id": scenario_id}

    # 4. Run analyze_longrun
    print("[+] Stage: analyze_longrun")
    analyze_result = subprocess.run(
        [
            sys.executable, "-m", "scripts.reporting.analyze_longrun",
            "--realtime", realtime_path,
            "--archive", archive_path,
            "--output-dir", output_subdir,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if analyze_result.returncode not in (0, 1):
        # analyze_longrun returns 1 for FAIL, 2 for ERROR - we continue on FAIL but stop on ERROR
        print(f"[-] analyze_longrun failed with exit code {analyze_result.returncode}")
        print(analyze_result.stdout)
        print(analyze_result.stderr)
    # Don't fail on analyze_longrun alone - proceed to gate

    # 5. Run perf_gate (optional - requires policy)
    perf_policy = DEFAULT_PERF_POLICY if os.path.isfile(DEFAULT_PERF_POLICY) else None
    if perf_policy and os.path.isfile(archive_path):
        print("[+] Stage: perf_gate")
        gate_result = subprocess.run(
            [
                sys.executable, "-m", "scripts.reporting.perf_gate",
                "--candidate", archive_path,
                "--policy", perf_policy,
                "--output-dir", output_subdir,
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if gate_result.returncode == 2:
            print(f"[-] perf_gate error: {gate_result.stderr}")
        elif gate_result.returncode != 0:
            print(f"[*] perf_gate returned {gate_result.returncode}")

    # 6. Run plot_report (optional)
    if not skip_plot:
        print("[+] Stage: plot_report")
        detail_csv = os.path.join(task_dir, "detail.csv")
        plot_result = subprocess.run(
            [sys.executable, "-m", "scripts.reporting.plot_report"],
            check=False,
            capture_output=True,
            text=True,
        )
        if plot_result.returncode != 0:
            print(f"[*] plot_report returned {plot_result.returncode}: {plot_result.stderr}")

    return {"exit_code": 0, "task_dir": task_dir, "scenario_id": scenario_id}


def run_suite(
    suite_path: str,
    output_subdir: str,
    binary: str,
    skip_plot: bool,
) -> Dict[str, Any]:
    """Execute a suite of scenarios."""
    from scripts.suites.resolve_suite import (
        build_default_settings,
        build_profile_map,
        expand_matrix,
        load_suite,
        materialize_scenario,
    )

    suite_dir = os.path.dirname(os.path.abspath(suite_path))
    doc = load_suite(suite_path)
    suite_id = doc.get("suite_id") or os.path.splitext(os.path.basename(suite_path))[0]

    defaults = build_default_settings(doc, suite_dir)
    profiles = build_profile_map(doc, suite_dir)

    scenarios = expand_matrix(
        suite_id, suite_dir, defaults, profiles, doc.get("matrix") or {}
    )
    for raw in doc.get("scenarios") or []:
        scenarios.append(materialize_scenario(
            suite_id, suite_dir, defaults, profiles, raw, ""
        ))

    results = []
    for scenario in scenarios:
        if not scenario.get("enabled", True):
            continue
        sid = scenario.get("scenario_id", "unknown")
        print(f"[+] Running scenario: {sid}")
        # Write temporary config for this scenario
        import tempfile
        import yaml
        temp_config = tempfile.NamedTemporaryFile(mode='w', suffix='.yaml', delete=False)
        try:
            yaml.safe_dump(scenario, temp_config)
            temp_config.close()
            result = run_single_scenario(
                temp_config.name, output_subdir, binary, skip_plot, sid
            )
            results.append(result)
            if result["exit_code"] != 0 and not scenario.get("continue_on_fail", True):
                print(f"[-] Scenario {sid} failed, stopping suite execution")
                break
        finally:
            os.unlink(temp_config.name)

    return {"scenarios": results}


def build_summary(run_id: str, all_results: List[Dict[str, Any]], output_dir: str) -> None:
    """Write summary.json and summary.md to output_dir."""
    passed = sum(1 for r in all_results if r.get("exit_code") == 0)
    failed = sum(1 for r in all_results if r.get("exit_code") != 0)

    summary_json = {
        "run_id": run_id,
        "total_scenarios": len(all_results),
        "passed": passed,
        "failed": failed,
        "scenarios": [
            {"scenario_id": r.get("scenario_id"), "exit_code": r.get("exit_code")}
            for r in all_results
        ],
        "output_dir": output_dir,
    }

    json_path = os.path.join(output_dir, "summary.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(summary_json, f, indent=2)

    md_lines = [
        "# Benchmark Run Summary\n",
        f"- Run ID: {run_id}",
        f"- Total Scenarios: {len(all_results)}",
        f"- Passed: {passed} | Failed: {failed}\n",
        "## Scenario Results\n",
        "| Scenario | Exit Code |",
        "| --- | --- |",
    ]
    for r in all_results:
        md_lines.append(f"| {r.get('scenario_id', 'unknown')} | {r.get('exit_code')} |")

    md_path = os.path.join(output_dir, "summary.md")
    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(md_lines))

    print(f"[+] summary.json -> {json_path}")
    print(f"[+] summary.md   -> {md_path}")


def run(
    config_path: Optional[str],
    suite_path: Optional[str],
    output_dir: str,
    run_id: Optional[str],
    skip_plot: bool,
    benchmark_binary: Optional[str],
) -> int:
    """Main entry point called from CLI."""
    binary = resolve_benchmark_binary(benchmark_binary)
    effective_run_id = run_id or generate_run_id(None)

    dirs = ensure_output_dirs(effective_run_id, output_dir)
    for path in dirs.values():
        os.makedirs(path, exist_ok=True)

    if config_path:
        scenario_id = os.path.basename(config_path).replace(".yaml", "").replace(".yml", "")
        results = [run_single_scenario(
            config_path, dirs["benchmark"], binary, skip_plot, scenario_id
        )]
    elif suite_path:
        result = run_suite(suite_path, dirs["benchmark"], binary, skip_plot)
        results = result.get("scenarios", [])

    build_summary(effective_run_id, results, dirs["root"])

    all_passed = all(r.get("exit_code") == 0 for r in results)
    return 0 if all_passed else 1
