# Architecture Research

**Domain:** Python script orchestration with C benchmarking tool
**Researched:** 2026-04-19
**Confidence:** HIGH

## Standard Architecture

### System Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     Orchestration Layer                      │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────┐ │
│  │  run_benchmark  │  │  merge_details  │  │ orchestrator │ │
│  │  (subprocess)   │  │  (Python CSV)   │  │ (workflow)   │ │
│  └────────┬────────┘  └────────┬────────┘  └──────┬───────┘ │
│           │                    │                   │         │
├───────────┴────────────────────┴───────────────────┴─────────┤
│                     C Benchmark Core                          │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐ │
│  │  main    │  │  worker  │  │  monitor │  │  obs_adapter │ │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘ │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼ (files: archive.csv, realtime.txt, detail_*.csv)
┌─────────────────────────────────────────────────────────────┐
│                   Python Reporting Layer                     │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐  ┌─────────────────┐  ┌────────────┐ │
│  │ analyze_longrun │  │   perf_gate      │  │ plot_report│ │
│  └─────────────────┘  └─────────────────┘  └────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### Component Responsibilities

| Component | Responsibility | Implementation |
|-----------|----------------|----------------|
| orchestrator | One-click workflow sequencing, pass outputs as inputs | Python main entry point (subprocess runner) |
| run_benchmark | Execute C binary with config, capture outputs | subprocess.run with cwd, env, timeout management |
| merge_details | Merge detail_*.csv into detail.csv | pandas.concat with sort |
| analyze_longrun | Compute RSS growth, TPS drift, success rate | Python analysis from realtime.txt + archive.csv |
| perf_gate | Compare candidate vs baseline archives | archive_compare_lib with threshold evaluation |
| plot_report | Generate 2x2 dashboard visualization | matplotlib with pandas aggregation |

## Recommended Project Structure

```
scripts/
├── orchestration/              # One-click workflow scripts
│   ├── oneclick.py             # Main orchestrator entry point
│   ├── run_benchmark.py        # subprocess wrapper for C binary
│   └── config_generator.py     # Suite template generation
├── sdk/                        # SDK management
├── reporting/                  # Existing analysis scripts (do not modify)
│   ├── merge_details.py
│   ├── analyze_longrun.py
│   ├── perf_gate.py
│   ├── plot_report.py
│   └── archive_compare_lib.py
├── suites/                     # Suite resolution (keep as-is)
│   └── resolve_suite.py
└── tests/
```

### Structure Rationale

- **orchestration/:** New code for REQ-02 (full-chain one-click). Isolated from existing reporting scripts to maintain backward compatibility.
- **reporting/:** Preserved unchanged per backward-compatibility constraint. Each script remains independently runnable.
- **orchestration/run_benchmark.py:** Thin subprocess wrapper - NOT embedded in reporting scripts. Provides consistent timeout, error handling, and output capture.

## Architectural Patterns

### Pattern 1: Subprocess Runner (Thin Wrapper)

**What:** Python function that invokes the C binary via subprocess with consistent timeout, env passthrough, and error handling.

**When to use:** Every orchestration script that needs to run the benchmark.

**Trade-offs:** Simple, portable, works across platforms. Does not provide real-time streaming of output (use Popen with threading for streaming).

**Example:**
```python
# scripts/orchestration/run_benchmark.py
import subprocess
import os
from pathlib import Path

def run_benchmark(
    config_path: str,
    binary_path: str = "./obs_c_bench",
    timeout_seconds: int = 3600,
    env_overrides: dict | None = None,
) -> subprocess.CompletedProcess:
    """
    Run obs_c_bench with consistent error handling.

    Returns CompletedProcess with returncode, stdout, stderr.
    Raises TimeoutExpired if benchmark exceeds timeout.
    """
    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)

    # Determine working directory from config path
    cwd = Path(config_path).parent.resolve()

    result = subprocess.run(
        [binary_path, "--config-file", str(config_path)],
        cwd=str(cwd),
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout_seconds,
    )
    return result
```

**Key points:**
- Always use absolute paths for config_file and binary
- Pass env variables explicitly (OBS_SDK_ROOT, LD_LIBRARY_PATH)
- Set timeout to prevent runaway processes (1 hour default for long-run tests)
- capture_output (Python 3.7+) instead of stdout=PIPE + stderr=PIPE separately

### Pattern 2: Pipeline Data Flow (File-Based Handoff)

**What:** Scripts communicate via intermediate files. Each script's output becomes the next script's input.

**When to use:** The natural data flow for this project (benchmark produces CSV/JSON, analysis consumes).

**Trade-offs:** Simple, debuggable, idempotent. Temporary files need cleanup strategy.

**Data Flow:**
```
C binary
  └─→ archive.csv ─┬─→ perf_gate.py
                   ├─→ analyze_longrun.py (needs realtime.txt too)
                   └─→ merge_details.py (needs detail_*.csv)
  └─→ realtime.txt ───→ analyze_longrun.py
  └─→ detail_*.csv ────→ merge_details.py ──→ detail.csv ──→ plot_report.py
```

**Implementation:**
```python
# Each script accepts explicit paths, does not auto-discover
# Auto-discovery only in plot_report.py (last in chain, user convenience)
def run_pipeline(task_dir: Path, config: PipelineConfig):
    # Step 1: Benchmark
    result = run_benchmark(config.config_path, binary_path=config.binary_path)
    if result.returncode != 0:
        raise PipelineError(f"Benchmark failed: {result.stderr}")

    # Step 2: Merge details (always, needed for plot_report)
    merge_latest_task_logs(task_dir)

    # Step 3: Analysis (if longrun)
    if config.run_longrun_analysis:
        analyze_longrun(
            realtime=task_dir / "realtime.txt",
            archive=task_dir / "archive.csv",
            output_dir=task_dir / "longrun",
        )

    # Step 4: Perf gate (if baseline comparison requested)
    if config.baseline_path:
        perf_gate(
            baseline=config.baseline_path,
            candidate=task_dir / "archive.csv",
            policy=config.gate_policy_path,
            output_dir=task_dir / "gate",
        )

    # Step 5: Dashboard (always, last step)
    generate_dashboard(task_dir / "detail.csv")
```

### Pattern 3: Orchestrator with Explicit Exit Codes

**What:** Main entry point that chains scripts, propagates failures via exit codes.

**When to use:** For the one-click workflow (REQ-02).

**Trade-offs:** Clear failure modes, CI-friendly. Each step must handle its own errors and decide whether to continue.

**Example:**
```python
#!/usr/bin/env python3
# scripts/orchestration/oneclick.py
import sys
from pathlib import Path

def main():
    import argparse
    parser = argparse.ArgumentParser(description="One-click benchmark pipeline")
    parser.add_argument("--config", required=True, help="Path to config file or suite YAML")
    parser.add_argument("--baseline", help="Baseline archive for comparison")
    parser.add_argument("--output-dir", help="Output directory (default: logs/task_*)")
    parser.add_argument("--timeout", type=int, default=3600)
    args = parser.parse_args()

    task_dir = Path(args.output_dir) if args.output_dir else None

    # Run benchmark
    result = run_benchmark(args.config, timeout_seconds=args.timeout)
    if result.returncode != 0:
        print(f"[-] Benchmark failed with code {result.returncode}", file=sys.stderr)
        return 1

    # Discover latest task directory
    if task_dir is None:
        task_dirs = sorted(Path("logs").glob("task_*"))
        if not task_dirs:
            print("[-] No task directories found", file=sys.stderr)
            return 1
        task_dir = task_dirs[-1]

    # Chain subsequent steps
    step = 1
    if merge_details(str(task_dir / "detail.csv")) == 0:
        step += 1

    if args.baseline:
        gate_result = perf_gate(args.baseline, task_dir / "archive.csv")
        if gate_result != 0:
            print(f"[*] Gate check: {gate_result}", file=sys.stderr)
            # Continue to plotting even if gate fails

    plot_result = generate_dashboard(task_dir / "detail.csv")
    return 0 if plot_result == 0 else 1
```

### Pattern 4: Template-Based Config Generation (REQ-01)

**What:** Python script that generates scenario YAML from simplified user input.

**When to use:** For REQ-01 (simplified scenario configuration experience).

**Trade-offs:** Reduces user-facing complexity. Adds generation layer.

**Not covered here:** This is REQ-01 scope, not REQ-02 (orchestration). See FEATURES.md for details.

## Data Flow

### Request Flow

```
User Action: ./oneclick.py --config scenario.yaml
    │
    ▼
Orchestrator: resolve_suite.py (if suite YAML)
    │
    ▼
C Benchmark: subprocess.run → archive.csv, realtime.txt, detail_*.csv
    │
    ▼
Python Scripts Chain:
    merge_details → detail.csv
    analyze_longrun → longrun_summary.json/md (optional)
    perf_gate → compare.csv, gate_result.json (optional)
    plot_report → dashboard.png
    │
    ▼
Final Output: dashboard.png + optional JSON/MD reports
```

### State Management

Benchmark state lives on disk:
- **Configuration:** YAML or .dat files (input)
- **Results:** `logs/task_YYYYMMDD_HHMMSS/` directory per run
- **Baselines:** `ci/perf/baseline_archives/` directory

No in-memory state shared between scripts. Each script is stateless, operates on files.

### Key Data Flows

1. **Benchmark execution:** Config file → C binary → CSV/ TXT output files
2. **Merge pipeline:** detail_*.csv → merged detail.csv (sorted by timestamp)
3. **Longrun analysis:** realtime.txt + archive.csv → longrun_summary.json + .md
4. **Baseline comparison:** candidate archive.csv + baseline archive.csv → compare.csv + gate_result.json
5. **Visualization:** detail.csv → 2x2 dashboard PNG

## Scaling Considerations

| Scale | Architecture Adjustments |
|-------|--------------------------|
| 0-1K requests | Current architecture sufficient |
| 1K-100K requests | merge_details downsampling (already implemented in plot_report.py) |
| 100K+ requests | Consider streaming CSV processing, chunked analysis |

### Scaling Priorities

1. **First bottleneck:** detail.csv merge (1000+ files with BATCH_SIZE=1000). Already handled via pandas concat.
2. **Second bottleneck:** plot_report downsampling (MAX_SCATTER_POINTS=100000). Already implemented.
3. **Memory:** analyze_longrun loads all realtime samples. For very long runs, consider sliding window.

## Anti-Patterns

### Anti-Pattern 1: Embedding Subprocess Calls in Analysis Scripts

**What people do:** Adding `subprocess.run(["./obs_c_bench", ...])` directly into `analyze_longrun.py` or `perf_gate.py`.

**Why it's wrong:** Violates single responsibility. Makes scripts harder to test independently. Creates coupling between reporting and execution.

**Do this instead:** Keep subprocess in dedicated orchestration layer (`run_benchmark.py`). Reporting scripts accept file paths only.

### Anti-Pattern 2: Implicit Output Directory Discovery Across All Scripts

**What people do:** Every script auto-discovers latest task directory via `glob.glob("logs/task_*")[-1]`.

**Why it's wrong:** Works for simple cases but breaks in CI, parallel runs, or when output is in non-standard location. Makes script behavior unpredictable.

**Do this instead:** Scripts accept explicit paths. Auto-discovery only in the final user-facing script (`plot_report.py`) where convenience outweighs risk.

### Anti-Pattern 3: Mixing Workflow Logic with Analysis Logic

**What people do:** Adding `--run-benchmark`, `--compare-baseline`, `--generate-plots` flags to existing scripts.

**Why it's wrong:** Bloats scripts with concerns they were not designed for. Violates backward compatibility.

**Do this instead:** Create new orchestration layer. Existing scripts remain unchanged.

### Anti-Pattern 4: Silent Failure and Continuation

**What people do:** Catching all exceptions, printing warning, continuing pipeline.

**Why it's wrong:** Hides real failures. Makes debugging harder. CI may not catch regressions.

**Do this instead:** Fail fast on unrecoverable errors. Provide exit codes. Continue only on optional steps (e.g., plot_report after gate failure).

## Integration Points

### External Services

| Service | Integration Pattern | Notes |
|---------|---------------------|-------|
| OBS SDK | Linked binary (libeSDKOBS.so/.a) | Platform-specific, via Makefile |
| File System | Read/write CSV, YAML, JSON | Primary integration mechanism |

### Internal Boundaries

| Boundary | Communication | Notes |
|----------|---------------|-------|
| Orchestrator → C binary | subprocess.run | Files as output |
| C binary → Python scripts | File system (CSV/TXT) | Time-ordered by Timestamp(s) column |
| merge_details → plot_report | File system (detail.csv) | merge must run first |
| perf_gate → analyze_longrun | None | Independent, can run parallel |

## Build Order Implications

1. **C binary must be compiled before orchestration works.** Add `make` check in orchestrator or document as prerequisite.

2. **Python dependencies (pandas, matplotlib, numpy, PyYAML) must be installed.** Document in README or setup.py.

3. **Recommended build sequence in orchestrator:**
```
1. Check C binary exists → error if not found
2. Check Python deps importable → error with install hint if not
3. Parse config (YAML or .dat)
4. Run benchmark via subprocess
5. Chain Python reporting scripts
```

## Sources

- Python subprocess documentation: https://docs.python.org/3/library/subprocess.html
- Existing codebase patterns: `scripts/reporting/*.py`, `Makefile`
- Project constraints: `.planning/PROJECT.md` (backward compatibility, one-click experience)
