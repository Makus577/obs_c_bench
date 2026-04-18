# Project Research Summary

**Project:** OBS C Benchmark Tool Enhancement
**Domain:** Benchmark Tooling Enhancement (C-based OBS benchmark with Python reporting)
**Researched:** 2026-04-19
**Confidence:** MEDIUM

## Executive Summary

This project enhances an existing C-based OBS SDK benchmark tool with Python reporting scripts. The goal is reducing user friction through simplified configuration (Template 1), a one-click end-to-end pipeline (Template 2), extended long-run stability metrics (Template 3), and multi-metric CI gates (Template 4). The existing tool already covers all table-stakes features; enhancement focuses on reducing friction, not adding fundamental capabilities.

Experts build benchmark tooling with a clear separation: thin orchestration wrappers around battle-tested cores, file-based inter-process communication, and additive metrics that don't break existing gates. The recommended stack uses Click for CLI orchestration, PyYAML for config, and preserves existing C binary and Python reporting scripts unchanged for backward compatibility.

Key risks include: template-generated configs hiding behavioral differences from manual configs, one-click workflows masking partial failures through silent continuations, and metric bloat obscuring regression signals. These risks are manageable with explicit validation, input checking, and additive-only metric policies.

## Key Findings

### Recommended Stack

**Core technologies:**
- **Click 8.x** — CLI framework with decorator-based command grouping; preferred over argparse for complex multi-command CLIs
- **PyYAML 6.x** — YAML config parsing with `safe_load`; prevents arbitrary code execution
- **Python 3.8+** — Project runtime; use `typing_extensions.Annotated` for 3.8 compatibility
- **pandas/matplotlib/numpy** — Already in project dependencies; use for CSV analysis and visualization

**Supporting patterns:**
- Wrapper approach: wrap existing scripts rather than rewriting them
- Subprocess runner: thin wrapper for C binary invocation with timeout and error handling
- File-based handoff: scripts communicate via intermediate files (archive.csv, realtime.txt, detail.csv)

### Expected Features

**Must have (table stakes):**
- Configurable workload parameters (threads, object_size, duration) — implemented
- Throughput metrics (TPS, BPS) — implemented
- Latency metrics (avg, P99, P99.9) — implemented via online histogram
- Success/failure rate tracking — implemented
- Machine-readable output (CSV/JSON) — implemented (archive.csv, brief.txt)
- Human-readable summary — implemented (brief.txt)
- Real-time progress visibility — implemented (console + realtime.txt)
- Multiple operation types — implemented (upload/download/delete/multipart/resumable/mix)
- Baseline comparison — implemented (perf_gate.py + baselines.csv)
- Basic visualization — implemented (plot_report.py + dashboard.png)

**Should have (competitive differentiators):**
- One-click end-to-end pipeline — chains obs_c_bench → merge_details → analyze_longrun → perf_gate → plot_report
- Configuration template generator — reduces 7 core concepts to 3-4 fields for common cases
- Smart parameter inheritance — users only override what's different from defaults
- RSS leak detection with trend analysis — detects steady memory growth
- TPS degradation detection — compares early vs late window throughput
- Multi-metric CI gates — memory, CPU, success rate alongside TPS
- Time-series trend visualization — metrics evolved over multi-hour runs

**Defer (v2+):**
- Cross-run trend tracking — requires baseline version history beyond adjacent-pair comparison
- Real-time streaming dashboards — requires WebSocket/server infrastructure

### Architecture Approach

The system uses a 3-layer architecture: Orchestration (Python subprocess runner) → C Benchmark Core (main, worker, monitor, obs_adapter) → Python Reporting (merge_details, analyze_longrun, perf_gate, plot_report). Components communicate via file-based handoff: archive.csv, realtime.txt, detail_*.csv. No in-memory state shared between scripts; each script is stateless and accepts explicit file paths.

**Major components:**
1. **orchestrator/oneclick.py** — Main entry point for one-click workflow; chains scripts with explicit exit codes
2. **orchestrator/run_benchmark.py** — Thin subprocess wrapper for C binary; provides consistent timeout, env passthrough, error handling
3. **orchestrator/config_generator.py** — Template-based scenario YAML generation; reduces user-facing complexity
4. **reporting/*.py** — Existing scripts preserved unchanged for backward compatibility

### Critical Pitfalls

1. **Template hides behavioral differences** — Simplified templates fill smart defaults users don't understand; must include `--explain` flag showing what was auto-filled and why
2. **One-click masks partial failures** — Sequential scripts silently skip steps when intermediate steps fail; must validate input files exist and use run_id to detect cross-run contamination
3. **Metric bloat obscures regression signal** — More metrics = more thresholds = more false failures; new metrics must be additive for at least one release
4. **Backward compatibility gaps** — New YAML fields may be silently ignored by C config_loader; must validate simplified configs against all existing code paths
5. **Memory growth in long-run analysis** — analyze_longrun.py loads all CSV into memory; must use chunked reading for large runs
6. **Dashboard proliferation without version control** — Generated dashboards diverge; must embed run metadata in filename and store templates in version control
7. **CI gate bypass** — Loose thresholds allow regressions through; must use statistical significance testing, not just threshold comparison
8. **Simplified config still requires expert knowledge** — Abstraction without education delays learning curve; must include inline context-sensitive help and `--interactive` mode

## Implications for Roadmap

Based on research, suggested phase structure:

### Phase 1: Template Generator + Smart Defaults
**Rationale:** Must come first — other phases depend on valid configuration. This reduces user friction immediately and surfaces hidden complexity early.
**Delivers:** Configuration template generator with `--explain` flag, smart parameter inheritance, inline context-sensitive help
**Addresses:** REQ-01 (simplified config), REQ-04 (smart defaults)
**Avoids:** Pitfalls 1 (template hides behavioral diffs), 4 (backward compat gaps), 8 (simplified config still complex)
**Stack elements:** Click decorators, PyYAML safe_load, template-based generation pattern

### Phase 2: One-Click Workflow + Dashboard Versioning
**Rationale:** Depends on Phase 1 (valid config required for pipeline). Integrates existing scripts without rewriting them.
**Delivers:** Single-command chain: benchmark → merge → analyze → compare → plot; input validation at each step; stale-data detection; run_id propagation
**Addresses:** REQ-02 (one-click pipeline), dashboard versioning
**Avoids:** Pitfalls 2 (one-click masks failures), 6 (dashboard proliferation)
**Implements:** orchestrator/oneclick.py with explicit exit codes

### Phase 3: Extended Metrics + Statistical Gates
**Rationale:** Final phase — builds on existing analysis infrastructure. New metrics must be additive-only initially.
**Delivers:** Enhanced RSS leak detection with trend lines, TPS degradation analysis, multi-metric CI gates with statistical significance testing, memory-bounded processing
**Addresses:** REQ-03 (extended longrun metrics), REQ-05 (multi-metric CI gates)
**Avoids:** Pitfalls 3 (metric bloat), 5 (memory growth), 7 (CI gate bypass)
**Stack elements:** pandas streaming, matplotlib with Agg backend, statistical testing

### Phase Ordering Rationale

- **Phase 1 before Phase 2:** Template generator establishes valid config that one-click pipeline consumes; no point automating broken config
- **Phase 2 before Phase 3:** One-click pipeline provides infrastructure for metric collection; extended metrics build on analyze_longrun.py (already exists)
- **Grouping by dependency:** Each phase's outputs feed the next; loose coupling via file-based handoff
- **Pitfall alignment:** Each phase explicitly addresses pitfalls discovered in research

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 3:** Statistical threshold methodology — may need research into appropriate statistical tests (Mann-Whitney U vs t-test) and baseline establishment procedures
- **Phase 3:** Memory-bounded CSV processing — may need research into pandas chunked reading best practices for 10M+ row files

Phases with standard patterns (skip research-phase):
- **Phase 1:** Template generation — well-documented Click decorator patterns
- **Phase 2:** Orchestrator with subprocess — standard Python patterns, documented in ARCHITECTURE.md

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | MEDIUM | Based on Context7 library docs, existing project dependencies; no external web search available |
| Features | MEDIUM | Based on project documentation and domain knowledge; external benchmark tool comparison unavailable due to no API keys |
| Architecture | HIGH | Based on existing codebase analysis; clear patterns from scripts/reporting/*.py and Makefile |
| Pitfalls | MEDIUM | Based on SRE best practices, Google Benchmark docs, existing CONCERNS.md; some inferred patterns may not apply |

**Overall confidence:** MEDIUM

### Gaps to Address

- **External validation missing:** Could not compare against industry benchmark tools (JMeter, Gatling, wrk2) due to unavailable web search. Recommend validation against JMeter plugins for long-run stability patterns.
- **Statistical methodology unspecified:** Phase 3 gates need research into appropriate statistical tests for TPS variance. Current suggestion (Mann-Whitney U) needs validation against CI/CD practices.
- **Memory bounds not established:** No empirical data on what constitutes "normal" archive.csv size for this tool. Recommend running load tests to establish thresholds.

## Sources

### Primary (HIGH confidence)
- Project existing scripts: `scripts/reporting/*.py`, `Makefile` — verified existing patterns
- Python subprocess documentation — standard library, reliable

### Secondary (MEDIUM confidence)
- Click Context7 (pallets/click) — CLI command patterns, option decorators
- PyYAML Context7 (yaml/pyyaml) — Safe YAML loading
- Google Benchmark User Guide — variance reduction, optimization prevention
- Google SRE Book — monitoring, alerting, testing reliability
- Grafana Best Practices — dashboard design

### Tertiary (LOW confidence)
- Domain knowledge of benchmark tooling patterns — inferred, needs validation against actual JMeter/Gatling users
- Pitfall patterns from CONCERNS.md — project-specific, may not generalize

---
*Research completed: 2026-04-19*
*Ready for roadmap: yes*
