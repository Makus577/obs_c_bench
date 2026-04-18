---
phase: "01"
plan: "01"
subsystem: configuration-simplification
tags:
  - cli
  - click
  - template-generator
  - smoke-test
dependency_graph:
  requires: []
  provides:
    - scripts/cli/__init__.py
    - scripts/cli/__main__.py
    - scripts/cli/template.py
    - scripts/cli/defaults.py
    - scripts/cli/validators.py
    - scripts/cli/templates/smoke.yaml
    - scripts/cli/templates/perf.yaml
    - scripts/cli/templates/longrun.yaml
  affects:
    - scripts/suites/resolve_suite.py
tech_stack:
  added:
    - Click 8.x (CLI framework)
    - PyYAML 6.x (template loading)
  patterns:
    - Click group/subcommand pattern
    - Template YAML loading with safe_load
    - 5-column provenance table for explain mode
    - Parameter validation with regex patterns
    - Deep merge for defaults resolution
key_files:
  created:
    - scripts/cli/__init__.py
    - scripts/cli/__main__.py
    - scripts/cli/template.py
    - scripts/cli/defaults.py
    - scripts/cli/validators.py
    - scripts/cli/templates/smoke.yaml
    - scripts/cli/templates/perf.yaml
    - scripts/cli/templates/longrun.yaml
decisions:
  - "Used Click 8.x as CLI framework (per STACK.md)"
  - "Templates use same structure as ci/perf/suites/examples/smoke_check.yaml for backward compatibility"
  - "Generated scenario YAML goes into scenarios[0] block; defaults/profiles/profiles preserved from template"
  - "resolve_effective_config kept in defaults.py but not called by generate_config to avoid over-merging (scenario keys were clobbering template structure)"
  - "explain_config generates 5-column Markdown table: 字段|来源|值|说明|生效状态"
metrics:
  duration: "288s"
  completed: "2026-04-19"
  tasks_completed: 3
  files_created: 8
---

# Phase 1 Plan 1: Configuration Simplification - CLI Template Generator

## One-Liner

Click-based CLI template generator producing valid scenario YAML from 3 required inputs (op, threads, object_size) with smart defaults and 5-column --explain provenance output.

## Commits

| Task | Commit | Files |
|------|--------|-------|
| Task 1: CLI skeleton | `1e1a6a2` | scripts/cli/__init__.py, scripts/cli/__main__.py |
| Task 2: Template generator | `24ba98c` | scripts/cli/template.py, scripts/cli/templates/{smoke,perf,longrun}.yaml |
| Task 3: Validators + defaults | `f4ce933` | scripts/cli/validators.py, scripts/cli/defaults.py |

## What Was Built

### CLI Entry Point
```bash
python -m scripts.cli template --op upload --threads 128 --object-size 1MB
python -m scripts.cli template --op upload --threads 128 --object-size 1MB --explain
python -m scripts.cli template --op upload --threads 128 --object-size 1MB --output scenario.yaml
python -m scripts.cli template --wizard
```

### Generated YAML Structure
```yaml
suite_id: smoke_template
defaults:
  users_file: ./users.dat
  requests_per_thread: 5
profiles:
  base:
    config_file: ./config.dat
scenarios:
  - scenario_id: smoke_template_upload_1mb_128t
    profile: base
    op: upload
    threads: 128
    object_size: 1MB
```

### 5-Column Explain Table
```
| 字段 | 来源 | 值 | 说明 | 生效状态 |
|------|------|-----|------|---------|
| users_file | default (from smoke template) | ./users.dat | Template default | ✓ |
| requests_per_thread | default (from smoke template) | 5 | Template default | ✓ |
| op | CLI | upload | 用户指定 | ✓ |
| threads | CLI | 128 | 用户指定 | ✓ |
| object_size | CLI | 1MB | 用户指定 | ✓ |
| scenario_id | auto | smoke_template_upload_1mb_128t | 自动生成 | ✓ |
| profile | template | base | 继承自模板 | ✓ |
| config_file | template | ./config.dat | 继承自模板 | ✓ |
```

## Verification Results

| Check | Command | Result |
|-------|---------|--------|
| CLI help | `python3 -m scripts.cli --help` | PASS - Shows template subcommand |
| Template generation | `python3 -m scripts.cli template --op upload --threads 128 --object-size 1MB` | PASS - Valid YAML output |
| Explain flag | `python3 -m scripts.cli template --op upload --threads 128 --object-size 1MB --explain` | PASS - 5-column table shown |
| File output | `python3 -m scripts.cli template --op upload --threads 128 --object-size 1MB --output /tmp/test.yaml` | PASS - File written |
| Validation errors | `python3 -m scripts.cli template --op invalid --threads 0 --object-size ""` | PASS - Click catches op; validator catches threads/object_size |
| Validator empty errors | `python3 -c "from scripts.cli.validators import validate_params; print(validate_params('upload', 128, '1MB'))"` | PASS - Returns [] |
| Validator 3 errors | `python3 -c "from scripts.cli.validators import validate_params; print(validate_params('invalid_op', 0, ''))"` | PASS - Returns 3 error strings |
| resolve_suite compat | `python3 scripts/suites/resolve_suite.py --suite /tmp/smoke_gen.yaml` | PASS - "[+] Suite: smoke_template\n[+] Scenarios: 1" |
| Perf template | `python3 -m scripts.cli template --op download --threads 64 --object-size 4MB --template perf` | PASS |
| Longrun template | `python3 -m scripts.cli template --op upload --threads 64 --object-size 4MB --template longrun` | PASS - Includes analyze_longrun: true |

## Deviations from Plan

### Rule 2 - Auto-added critical functionality

**1. Fixed broken generate_config merge that destroyed template structure**
- **Found during:** Task 2 verification
- **Issue:** `resolve_effective_config()` was deep-merging template defaults into the scenario block, flattening the YAML structure (moving defaults keys like `users_file` into the `scenarios[0]` dict)
- **Fix:** Removed the `resolve_effective_config()` call from `generate_config()` and removed the unused import. Template structure is preserved as-is with user overrides applied only to the specific scenario fields (op, threads, object_size)
- **Files modified:** scripts/cli/template.py
- **Commit:** `24ba98c` (amended in `f4ce933`)

## Deviations from Plan

### Rule 2 - Auto-added critical functionality

**1. Fixed CLI import structure for top-level generate_config/explain_config references**
- **Found during:** Task 1 verification
- **Issue:** Initial `__main__.py` had broken top-level imports that would fail at runtime since `template.py` did not exist yet
- **Fix:** Moved imports inside the function that used them and added a top-level import from `scripts.cli.template` at module level to make the functions accessible
- **Files modified:** scripts/cli/__main__.py
- **Commit:** `1e1a6a2`

## Threat Surface Scan

| Flag | File | Description |
|------|------|-------------|
| N/A | - | No new trust boundaries crossed; all inputs are CLI args validated before use; yaml.safe_dump used (no arbitrary code execution); template paths are internal to package |

## Self-Check

- [x] scripts/cli/__init__.py exists
- [x] scripts/cli/__main__.py exists
- [x] scripts/cli/template.py exists
- [x] scripts/cli/validators.py exists
- [x] scripts/cli/defaults.py exists
- [x] scripts/cli/templates/smoke.yaml exists
- [x] scripts/cli/templates/perf.yaml exists
- [x] scripts/cli/templates/longrun.yaml exists
- [x] Commit `1e1a6a2` exists
- [x] Commit `24ba98c` exists
- [x] Commit `f4ce933` exists
- [x] No files were deleted by commits

## Self-Check: PASSED
