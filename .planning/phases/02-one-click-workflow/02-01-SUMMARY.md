---
phase: "02"
plan: "01"
subsystem: orchestration
tags:
  - one-click-workflow
  - cli
  - orchestration
  - pipeline
dependency_graph:
  requires: []
  provides:
    - scripts/orchestration/run_benchmark.py
    - scripts/orchestration/dry_run.py
  affects:
    - scripts/cli/__main__.py
tech_stack:
  added:
    - Python subprocess orchestration
    - Click CLI multi-command group
    - YAML validation utilities
  patterns:
    - Fail-fast pipeline chaining
    - Auto-detecting binary resolution
    - Slugified run ID generation
key_files:
  created:
    - scripts/orchestration/run_benchmark.py
    - scripts/orchestration/dry_run.py
  modified:
    - scripts/cli/__main__.py
decisions:
  - "Binary auto-detection prefers obs_c_bench, falls back to obs_c_bench_mock"
  - "perf_gate skipped silently if no policy file found (not a hard error)"
  - "analyze_longrun returns non-zero on FAIL but continues to gate (exit codes 0/1 treated as continue)"
  - "Dry-run uses ValueError to signal validation failures (caught and displayed by CLI)"
---

# Phase 2 Plan 1: One-Click Workflow Summary

**One-liner:** Orchestration CLI `run` subcommand chaining benchmark -> merge -> analyze -> gate -> plot with fail-fast error handling.

## What Was Built

- `python -m scripts.cli run --config scenario.yaml --output-dir ./reports` chains the full pipeline
- `python -m scripts.cli run --suite suite.yaml` handles multi-scenario execution
- `--skip-plot` skips dashboard generation for faster debugging cycles
- `--dry-run` validates YAMLs without running any benchmark
- `--run-id` allows custom identifiers; auto-generated as `{slug}_{timestamp}`
- Output: `./reports/<run_id>/{benchmark,analysis,gate,dashboard}/` with `summary.json` + `summary.md`

## Tasks Executed

| # | Name | Commit | Files |
|---|------|--------|-------|
| 1 | Add run subcommand to CLI | `61daf9f` | scripts/cli/__main__.py |
| 2 | Implement orchestration engine | `f664411` | scripts/orchestration/run_benchmark.py |
| 3 | Implement dry-run validation | `65eb51b` | scripts/orchestration/dry_run.py |

## Verification

- `python3 -m scripts.cli run --help` shows all options (config, suite, output-dir, run-id, skip-plot, dry-run, benchmark-binary)
- `python3 -c "from scripts.orchestration.run_benchmark import generate_run_id; print(generate_run_id('my_scenario'))"` outputs `my_scenario_20260419_011156`
- Click's `exists=True` validates file paths before dry-run runs
- Mutual exclusivity enforced via Click validation before orchestration is invoked

## Deviations from Plan

None — plan executed as written.

## Threat Flags

None introduced beyond plan scope.

## Self-Check: PASSED

All files created exist. Commits verified in git log.
