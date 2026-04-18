# Roadmap: obs_c_bench Enhancement

## Phases

- [ ] **Phase 1: Configuration Simplification** — Template generator + smart defaults
- [ ] **Phase 2: One-Click Workflow** — End-to-end pipeline orchestration
- [ ] **Phase 3: Long-Run Enhancement + CI Gates** — Extended metrics and statistical gates

## Phase Details

### Phase 1: Configuration Simplification

**Goal:** Users can generate valid scenario configurations with minimal input

**Depends on:** None (first phase)

**Requirements:** REQ-01, REQ-04

**Success Criteria** (what must be TRUE):

1. User can generate a scenario config by specifying only core parameters (operation type, threads, object_size) and receive a valid, executable YAML
2. User can run `template --explain` to see which fields were auto-filled and why
3. User can use `defaults:` block to specify shared parameters once, with scenarios inheriting them
4. User receives validation errors for invalid parameters (threads <= 0, object_size <= 0) before execution

**Plans:** TBD

---

### Phase 2: One-Click Workflow

**Goal:** Users can execute the full benchmarking pipeline with a single command

**Depends on:** Phase 1

**Requirements:** REQ-02

**Success Criteria** (what must be TRUE):

1. User can run `python -m obs_bench.cli run --config scenario.yaml --suite suite.yaml --output-dir ./reports` and see all stages execute sequentially
2. User sees non-zero exit code and clear error message when any stage fails, without silent continuation
3. User can inspect `./reports/<run_id>/` containing {benchmark, analysis, gate, dashboard}/ subdirectories with all intermediate outputs
4. User can skip the plotting step with `--skip-plot` to save time when debugging gate failures

**Plans:** TBD

---

### Phase 3: Long-Run Enhancement + CI Gates

**Goal:** Users can analyze long-run stability with extended metrics and enforce multi-metric CI gates

**Depends on:** Phase 2

**Requirements:** REQ-03, REQ-05

**Success Criteria** (what must be TRUE):

1. User can view `longrun_summary.json` containing CPU drift percentage, TPS degradation ratio, and lowest success_rate timestamp
2. User can view `longrun_trend.png` showing RSS, TPS, and success_rate time-series on a single chart
3. User can define `gate_policy.json` with thresholds for memory (avg_rss_mb, peak_rss_mb), CPU (avg_cpu_pct, peak_cpu_pct), and bandwidth (avg_single_stream_bps, max_single_stream_bps)
4. User can see `statistical_significance` field in `perf_gate_result.json` when TPS comparisons use t-test or Mann-Whitney U (p < 0.05 indicates regression)

**Plans:** TBD

---

## Progress Table

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Configuration Simplification | 0/4 | Not started | - |
| 2. One-Click Workflow | 0/4 | Not started | - |
| 3. Long-Run Enhancement + CI Gates | 0/4 | Not started | - |

---

*Last updated: 2026-04-19*
