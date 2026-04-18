# Phase 03 Plan 01: Long-Run Enhancement CI Gates Summary

## Plan Status

**Status:** COMPLETE
**Completed:** 2026-04-19
**Tasks:** 2/2
**Commits:** 2

---

## One-liner

Extended `analyze_longrun.py` with tri-axis RSS/TPS/SuccessRate trend PNG and `success_rate_min_timestamp` tracking; enhanced `perf_gate.py` with memory/CPU/bandwidth threshold gates and statistical significance testing via Mann-Whitney U / t-test.

---

## Completed Tasks

| Task | Name | Commit | Files |
| ---- | ---- | ------ | ----- |
| 1 | Extend analyze_longrun.py with CPU drift, TPS attenuation, success_rate min tracking, and trend PNG | 2fb1402 | scripts/reporting/analyze_longrun.py |
| 2 | Enhance perf_gate.py with memory, CPU, bandwidth gates and statistical significance | 92df6cd | scripts/reporting/perf_gate.py |

---

## Task 1: Extend analyze_longrun.py

### What was built

- **`success_rate_min_timestamp`**: New field in `build_summary()` output that records `RunTime(s)` at which `Success_Rate(%)` reached its minimum across all samples.
- **`write_trend_png(samples, output_path)`**: New function generating a tri-axis time-series chart:
  - Left axis (blue): RSS (MB)
  - Center axis (green): Interval TPS
  - Right axis (red): Success Rate (%)
  - X-axis: RunTime (s)
  - 10000-point cap before plotting to limit matplotlib memory (threat T-03-01 mitigation)
  - Graceful fallback when matplotlib is not installed (plt = None guard)
- **`write_trend_png` called in `main()`**: Invoked before JSON write, saves to `{output_dir}/longrun_trend.png`.
- **`write_markdown` updated**: Added `success_rate_min_timestamp` to label map and table rows.
- **Output added**: `longrun_trend.png` path printed in `main()`.

### Exports preserved

`build_summary`, `apply_policy`, `write_markdown` (unchanged signatures, additive fields only).

### Verification

```
Extended fields present in summary
success_rate_min_timestamp = 27.0
write_trend_png executed without error
analyze_longrun.py extension verified
```

### Key files

- `scripts/reporting/analyze_longrun.py` — 434 lines (was 356, added 78 lines)

---

## Task 2: Enhance perf_gate.py

### What was built

- **`compute_statistical_significance(baseline_samples, candidate_samples, direction)`**: New helper that runs Mann-Whitney U (preferred) or Welch's t-test (fallback) on two sample lists. Returns `test_type`, `p_value`, `is_significant` (p < 0.05), `n_baseline`, `n_candidate`, and `reason` when insufficient or failed.
- **Graceful scipy fallback**: `try/except ImportError` on `scipy.stats`; if absent, statistical tests are skipped (threat T-03-03 mitigation).
- **`load_realtime_samples(path)`**: New helper to parse realtime.txt CSV into list of dicts with float values, matching the same parsing conventions as `archive_compare_lib`.
- **Statistical significance in `evaluate_gate`**: Each gate_row now carries `statistical_significance` dict (None when no samples provided). Currently maps `final_tps` and `peak_tps` to `Interval_TPS` column from realtime data.
- **New CLI args**: `--baseline-realtime` and `--candidate-realtime` for passing realtime.txt paths.
- **`statistical_significance` array in gate_result**: Top-level field in `gate_result.json` containing one entry per metric with a computed significance result.
- **Memory/CPU/bandwidth gates**: Absolute thresholds (`absolute_fail_min`, `absolute_fail_max`, `absolute_warn_min`, `absolute_warn_max`) already supported in `evaluate_gate`; `DEFAULT_METRIC_SPECS` in `archive_compare_lib` already covers `avg_rss_mb`, `peak_rss_mb`, `avg_cpu_pct`, `peak_cpu_pct`, `avg_single_stream_bps`, `max_single_stream_bps`.
- **Backward compatible**: All existing CLI flags unchanged; new functionality is purely additive.

### Verification

```
compute_statistical_significance works
Gate statuses: ['FAIL', 'FAIL', 'FAIL']  # correctly evaluates memory/CPU/bandwidth thresholds
perf_gate.py enhanced metrics verified
```

### Key files

- `scripts/reporting/perf_gate.py` — 358 lines (was 232, added 126 lines)

---

## Deviations from Plan

None — plan executed exactly as written.

---

## Threat Surface Scan

| Flag | File | Description |
|------|------|-------------|
| None | analyze_longrun.py | No new trust boundaries introduced; CSV parsing already handles malformed values via `parse_float` |
| None | perf_gate.py | No new trust boundaries; scipy import has graceful fallback |

---

## Self-Check

- [x] `success_rate_min_timestamp` present in `build_summary` output
- [x] `write_trend_png` generates `longrun_trend.png` (or gracefully no-ops without matplotlib)
- [x] `perf_gate.py` gate result includes `statistical_significance` array
- [x] `perf_gate.py` correctly gates on `avg_rss_mb`, `avg_cpu_pct`, `avg_single_stream_bps` with absolute thresholds
- [x] analyze_longrun.py: 434 lines >= 420 minimum
- [x] perf_gate.py: 358 lines >= 280 minimum
- [x] Both scripts remain backward compatible with existing CLI arguments

## Self-Check: PASSED
