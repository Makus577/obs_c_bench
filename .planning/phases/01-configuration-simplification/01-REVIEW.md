---
phase: 01-configuration-simplification
reviewed: 2026-04-19T00:00:00Z
depth: standard
files_reviewed: 8
files_reviewed_list:
  - scripts/cli/__init__.py
  - scripts/cli/__main__.py
  - scripts/cli/template.py
  - scripts/cli/defaults.py
  - scripts/cli/validators.py
  - scripts/cli/templates/smoke.yaml
  - scripts/cli/templates/perf.yaml
  - scripts/cli/templates/longrun.yaml
findings:
  critical: 0
  warning: 3
  info: 3
  total: 6
status: issues_found
---

# Phase 01: Code Review Report

**Reviewed:** 2026-04-19
**Depth:** standard
**Files Reviewed:** 8
**Status:** issues_found

## Summary

The CLI scaffolding for simplified scenario configuration has been reviewed. The code is generally well-structured using Click for CLI and PyYAML for template handling. Several issues were found: inconsistent type conversion error handling, code duplication with `resolve_suite.py`, missing operation validation, and dead code. No critical security vulnerabilities were identified. The use of `yaml.safe_load`, absence of `eval()`/shell injection vectors, and `copy.deepcopy` for template cloning are all correct.

## Warnings

### WR-01: Inconsistent type conversion fallback in resolve_effective_config

**File:** `scripts/cli/defaults.py:89-99`
**Issue:** When integer conversion fails, `threads` falls back to an integer (line 87: `HARD_CODED_DEFAULTS['threads']`), but `run_seconds` and `requests_per_thread` fall back to empty string `''` (lines 93, 99). This type inconsistency could cause downstream issues if code expects an integer.

**Fix:**
```python
# Line 93: change
    result['run_seconds'] = ''
# to
    result['run_seconds'] = HARD_CODED_DEFAULTS['run_seconds']

# Line 99: change
    result['requests_per_thread'] = ''
# to
    result['requests_per_thread'] = HARD_CODED_DEFAULTS['requests_per_thread']
```

### WR-02: Missing op validation in resolve_effective_config

**File:** `scripts/cli/defaults.py:74`
**Issue:** `normalize_op()` is called but the result is never validated against allowed values. Invalid operations (e.g., `multiparts`, `resumable`) will silently pass through without error.

**Fix:**
Add validation after normalization:
```python
result['op'] = normalize_op(result.get('op'))
if result['op'] not in ('upload', 'download', ''):
    raise ValueError(f"Invalid op '{result['op']}'. Must be 'upload' or 'download'.")
```

### WR-03: Silent data loss when both object_size and object_size_spec are provided

**File:** `scripts/cli/defaults.py:77-80`
**Issue:** The `object_size_spec` alias is only handled when `object_size` is absent. If a user provides both fields, `object_size_spec` is silently dropped without warning.

**Fix:**
```python
# Handle object_size vs object_size_spec alias
if 'object_size_spec' in result:
    if 'object_size' not in result:
        result['object_size'] = clean_text(result.pop('object_size_spec'))
    else:
        # Both present - log or warn (silently preferring object_size)
        result.pop('object_size_spec')
else:
    result['object_size'] = clean_text(result.get('object_size', ''))
```

## Info

### IN-01: Redundant function definitions

**File:** `scripts/cli/__init__.py:27-38`
**Issue:** `normalize_op` and `clean_text` are redefined in this module (lines 27-31 and 34-38), but identical functions already exist in `scripts.suites.resolve_suite` which is imported on line 15. This is code duplication.

**Fix:**
```python
# Remove lines 27-38 and use the imported functions directly:
# normalize_op and clean_text are already imported from resolve_suite
```

Note: The local `normalize_op` in `__init__.py` does lowercase the result (`str(value).strip().lower()`), but the one in `resolve_suite.py` (line 106-110) does not. Verify which behavior is correct and consolidate.

### IN-02: Dead code - unused validate_params wrapper

**File:** `scripts/cli/__main__.py:134-137`
**Issue:** The `validate_params` function at lines 134-137 is defined but never called. The same function is called directly from `scripts.cli.validators` in both `template()` (line 62) and `_run_wizard()` (line 104).

**Fix:**
```python
# Remove lines 134-137 entirely
def validate_params(op, threads, object_size):
    """Validate parameters. Imported here to avoid circular imports in wizard."""
    from scripts.cli.validators import validate_params as _vp
    return _vp(op, threads, object_size)
```

### IN-03: Hardcoded relative paths in YAML templates

**File:** `scripts/cli/templates/smoke.yaml`, `perf.yaml`, `longrun.yaml`
**Issue:** All templates use hardcoded relative paths (`./users.dat`, `./config.dat`). If the tool is invoked from a directory different from the template location, these paths may be invalid.

**Fix:**
Either document that templates must be invoked from the repository root, or make paths relative to the template file location by resolving them in `load_template()` or `generate_config()`.

---

_Reviewed: 2026-04-19_
_Reviewer: Claude (gsd-code-reviewer)_
_Depth: standard_
