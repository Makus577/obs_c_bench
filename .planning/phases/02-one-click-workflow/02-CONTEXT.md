# Phase 2: One-Click Workflow - Context

**Gathered:** 2026-04-19
**Status:** Ready for planning
**Mode:** Auto-generated (discuss skipped via workflow.skip_discuss)

<domain>
## Phase Boundary

Users can execute the full benchmarking pipeline with a single command. This phase delivers:
1. **Orchestration CLI** — `python -m obs_bench.cli run` that chains: benchmark → merge_details → analyze_longrun → perf_gate → plot_report
2. **Output directory structure** — `./reports/<run_id>/{benchmark,analysis,gate,dashboard}/`

</domain>

<decisions>
## Implementation Decisions

### Claude's Discretion
All implementation choices are at Claude's discretion — discuss phase was skipped per autonomous mode. Use ROADMAP phase goal, success criteria, and codebase conventions to guide decisions.

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `scripts/reporting/analyze_longrun.py` — existing analysis script to chain
- `scripts/reporting/merge_details.py` — existing merge script to chain
- `scripts/reporting/perf_gate.py` — existing gate script to chain
- `scripts/reporting/plot_report.py` — existing plotting script to chain
- Phase 1 CLI at `scripts/cli/__main__.py` — existing Click CLI pattern to extend

### Established Patterns
- Python CLI scripts use argparse or Click
- All reporting scripts accept `--output-dir` or similar for directory control
- Subprocess used for C binary invocation

### Integration Points
- New `run` subcommand in `scripts/cli/` extends Phase 1 CLI
- Chains existing Python scripts via subprocess
- C benchmark binary invoked via subprocess with config

</code_context>

<specifics>
## Specific Ideas

No specific requirements — discuss phase skipped. Refer to ROADMAP phase description and success criteria.

</specifics>

<deferred>
## Deferred Ideas

None — discuss phase skipped.

</deferred>
