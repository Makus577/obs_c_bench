---
phase: 02-one-click-workflow
reviewed: 2026-04-19T00:00:00Z
depth: standard
files_reviewed: 3
files_reviewed_list:
  - scripts/cli/__main__.py
  - scripts/orchestration/run_benchmark.py
  - scripts/orchestration/dry_run.py
findings:
  critical: 0
  warning: 3
  info: 4
  total: 7
status: issues_found
---

# Phase 02: Code Review Report

**Reviewed:** 2026-04-19
**Depth:** standard
**Files Reviewed:** 3
**Status:** issues_found

## Summary

Reviewed `scripts/cli/__main__.py`, `scripts/orchestration/run_benchmark.py`, and `scripts/orchestration/dry_run.py`. The code is well-structured with proper separation between CLI, orchestration, and validation layers. No security vulnerabilities or logic errors found. Several code-quality improvements were identified, primarily around robustness and consistency.

## Warnings

### WR-01: Import inside loop reduces performance

**File:** `scripts/orchestration/run_benchmark.py:200-201`
**Issue:** `import tempfile` and `import yaml` are executed inside the `for scenario in scenarios` loop in `run_suite()`. These imports are idempotent but wasteful -- they lookup and import on every iteration. Additionally, the import shadows any module-level imports, making it harder to reason about the module's dependencies.
**Fix:**
```python
# Move imports to the top of run_suite (with other suite imports), or
# better: move them to module level alongside the existing resolve_suite imports:
import tempfile
import yaml

def run_suite(...):
    ...
```

### WR-02: Silent binary resolution with no existence check

**File:** `scripts/orchestration/run_benchmark.py:33-42`
**Issue:** `resolve_benchmark_binary()` returns `DEFAULT_BINARY` even when neither the user-provided path nor the auto-detected candidates exist on disk. The calling code then runs `subprocess.run([binary, ...])` which will fail with a cryptic "No such file or directory" from the OS, not a helpful validation message.
**Fix:**
```python
def resolve_benchmark_binary(path: Optional[str]) -> str:
    if path:
        if os.path.isfile(path):
            return os.path.abspath(path)
        raise FileNotFoundError(f"Benchmark binary not found: {path}")
    for name in ("obs_c_bench", "obs_c_bench_mock"):
        candidate = os.path.join(BASE_DIR, name)
        if os.path.isfile(candidate):
            return candidate
    raise FileNotFoundError(
        f"No benchmark binary found. Tried: obs_c_bench, obs_c_bench_mock in {BASE_DIR}"
    )
```

### WR-03: `dry_run_validate` accepts both arguments as None

**File:** `scripts/orchestration/dry_run.py:9`
**Issue:** Function signature `dry_run_validate(config_path: str, suite_path: str)` does not indicate that either argument may be `None`. The body handles the case where both are `None` gracefully (errors list remains empty), but the calling code in `__main__.py:53-56` relies on this behavior without explicit documentation. This is fragile if the function is called from a different entry point.
**Fix:** Use `Optional[str]` for both parameters and add a guard at the top:
```python
def dry_run_validate(config_path: Optional[str], suite_path: Optional[str]) -> None:
    if not config_path and not suite_path:
        raise ValueError("At least one of config_path or suite_path must be provided")
    ...
```

## Info

### IN-01: Inconsistent `output_subdir` argument name

**File:** `scripts/orchestration/run_benchmark.py:66`
**Issue:** `run_single_scenario()` takes `output_subdir` but at line 114, 136 it is used as `--output-dir`. Looking at `run()` at line 278, `dirs["benchmark"]` is passed, which is `os.path.join(root, "benchmark")`. The naming is slightly confusing since `output_subdir` suggests it is a subdirectory within the run root, but it is actually the full per-stage directory.
**Fix:** Rename `output_subdir` to `stage_dir` or `analysis_dir` for clarity, or add a docstring clarifying that it receives `dirs["benchmark"]`.

### IN-02: Perf gate skipped silently when policy file absent

**File:** `scripts/orchestration/run_benchmark.py:128-129`
**Issue:** When `DEFAULT_PERF_POLICY` does not exist, perf gate is silently skipped with no message to the user. A user may expect perf gating to always run unless explicitly disabled.
**Fix:**
```python
perf_policy = DEFAULT_PERF_POLICY if os.path.isfile(DEFAULT_PERF_POLICY) else None
if perf_policy:
    print("[+] Stage: perf_gate")
    ...
else:
    print("[*] Stage: perf_gate skipped (policy file not found)")
```

### IN-03: `find_latest_task_dir` relies on sorting by name

**File:** `scripts/orchestration/run_benchmark.py:57-60`
**Issue:** `sorted(glob.glob(...))[-1]` returns the lexicographically latest `task_*` directory, not necessarily the most recently created one. On filesystems with slow clock resolution or in directories with many old task directories, the wrong directory could be selected. There is also a race condition if multiple benchmark runs are active simultaneously.
**Fix:** Use `os.path.getmtime()` to sort by modification time instead:
```python
task_dirs = glob.glob(os.path.join(BASE_DIR, "logs", "task_*"))
if task_dirs:
    return max(task_dirs, key=os.path.getmtime)
```

### IN-04: Inconsistent comment format for return code docstrings

**File:** `scripts/orchestration/run_benchmark.py:121`
**Issue:** Comment `analyze_longrun returns 1 for FAIL, 2 for ERROR` uses an internal convention that is not obvious to future readers. The comment does not describe the convention fully (e.g., what does 2+ mean? What does 0 mean?).
**Fix:** Expand the comment or replace with a docstring on `run_single_scenario`:
```python
# Exit code convention for analyze_longrun:
#   0 = analysis passed
#   1 = analysis completed but found FAIL condition (e.g., regression) — continue to gate
#   2+ = ERROR (e.g., file not found) — stop pipeline
```

---

_Reviewed: 2026-04-19_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
