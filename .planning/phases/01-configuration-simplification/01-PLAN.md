---
phase: 01
plan: "01"
type: execute
wave: 1
depends_on: []
files_modified:
  - scripts/cli/__init__.py
  - scripts/cli/__main__.py
  - scripts/cli/template.py
  - scripts/cli/defaults.py
  - scripts/cli/validators.py
  - scripts/cli/templates/smoke.yaml
  - scripts/cli/templates/perf.yaml
  - scripts/cli/templates/longrun.yaml
  - scripts/suites/resolve_suite.py
autonomous: true
requirements:
  - REQ-01
  - REQ-04
user_setup: []

must_haves:
  truths:
    - "User can generate a scenario config by specifying only --op, --threads, --object-size"
    - "User can run template --explain to see auto-filled fields with sources"
    - "User can use defaults: block in Suite YAML for shared parameters"
    - "User receives validation errors for invalid parameters (threads <= 0, object_size <= 0, invalid op)"
  artifacts:
    - path: "scripts/cli/__main__.py"
      provides: "CLI entry point for python -m obs_bench.cli"
      min_lines: 30
    - path: "scripts/cli/template.py"
      provides: "Template generator with smoke/perf/longrun templates"
      exports: ["generate_config", "explain_config"]
    - path: "scripts/cli/defaults.py"
      provides: "Defaults resolution engine (scenario overrides defaults)"
      exports: ["resolve_effective_config"]
    - path: "scripts/cli/validators.py"
      provides: "Parameter validation (threads > 0, object_size > 0, valid op)"
      exports: ["validate_params"]
    - path: "scripts/cli/templates/smoke.yaml"
      provides: "Smoke template (5 requests, 2 threads, 1MB)"
    - path: "scripts/cli/templates/perf.yaml"
      provides: "Perf template (100 requests, 64-128 threads, various sizes)"
    - path: "scripts/cli/templates/longrun.yaml"
      provides: "Longrun template (3600s, 64 threads, 4MB)"
  key_links:
    - from: "scripts/cli/template.py"
      to: "scripts/cli/templates/*.yaml"
      via: "loads template YAML"
      pattern: "yaml.safe_load.*templates"
    - from: "scripts/cli/defaults.py"
      to: "scripts/suites/resolve_suite.py"
      via: "imported and extended"
      pattern: "from.*resolve_suite import"
    - from: "scripts/cli/__main__.py"
      to: "scripts/cli/template.py"
      via: "click command"
      pattern: "@cli.command.*template"
---

<objective>
Implement Phase 1 Configuration Simplification: a Click-based CLI template generator that produces valid scenario YAML from minimal user input (op, threads, object_size), with smart defaults and a 5-column --explain output showing field provenance.
Purpose: Reduce user friction from ~15 fields to 3 required inputs.
Output: Working `python -m obs_bench.cli template` command with smoke/perf/longrun templates.
</objective>

<execution_context>
@$HOME/.claude/get-shit-done/workflows/execute-plan.md
</execution_context>

<context>
@.planning/phases/01-configuration-simplification/01-CONTEXT.md
@.planning/REQUIREMENTS.md
@scripts/suites/resolve_suite.py
@scripts/reporting/analyze_longrun.py

# Template reference structure (from smoke_check.yaml)
defaults:
  users_file: ../../../users.dat
  requests_per_thread: 5

profiles:
  base:
    config_file: ../../../config.dat

scenarios:
  - scenario_id: smoke_upload_1mb_2t
    profile: base
    op: upload
    threads: 2
    object_size: 1MB
</context>

<interfaces>
<!-- Key types and contracts the executor needs. Extracted from codebase. -->

From scripts/suites/resolve_suite.py (merge_dicts pattern):
```python
def merge_dicts(*parts: Dict[str, Any]) -> Dict[str, Any]:
    """Deep-merges dicts left-to-right; later keys override earlier."""
```

From scripts/reporting/analyze_longrun.py (DEFAULT_POLICY structure):
```python
DEFAULT_VALUES = {
    'threads': 64,
    'object_size': '1MB',
    'run_seconds': 300,
    'requests_per_thread': 0,
}
```

Existing smoke_check.yaml template fields:
- suite_id, defaults (users_file, requests_per_thread), profiles (base: config_file), scenarios (scenario_id, profile, op, threads, object_size), baseline, gates, reporting
</interfaces>

<tasks>

<task type="auto">
  <name>Task 1: Create CLI module skeleton with Click 8.x entry point</name>
  <files>scripts/cli/__init__.py, scripts/cli/__main__.py</files>
  <action>
Create the `scripts/cli/` directory as a Python package. Use Click 8.x for the CLI framework (per STACK.md).

**scripts/cli/__init__.py:**
- Empty file to make it a package

**scripts/cli/__main__.py:**
- Entry point: `python -m obs_bench.cli template [OPTIONS]`
- Use `@click.group()` with subcommand `template`
- Import template generator from `.template`
- No third-party packages beyond Click 8.x and PyYAML 6.x (both already in project deps)
- Follow the hybrid CLI pattern: single-command default, `--wizard` flag enables interactive mode

```python
# Key structure
import click

@click.group()
def cli():
    """OBS C Benchmark Tool — Simplified workflow orchestration"""
    pass

@cli.command()
@click.option('--op', type=click.Choice(['upload', 'download']), required=True, help='Operation type')
@click.option('--threads', type=int, required=True, help='Number of threads')
@click.option('--object-size', 'object_size', required=True, help='Object size (e.g., 1MB, 4MB)')
@click.option('--template', 'template_type', type=click.Choice(['smoke', 'perf', 'longrun']), default='smoke', help='Template type')
@click.option('--output', '-o', type=click.Path(), help='Output file (default: stdout)')
@click.option('--explain', is_flag=True, help='Show field provenance table')
@click.option('--wizard', is_flag=True, help='Interactive wizard mode')
def template(op, threads, object_size, template_type, output, explain, wizard):
    """Generate a scenario YAML from minimal input"""
    pass
```

Keep existing reporting scripts (`scripts/reporting/*.py`) unchanged and independently runnable per backward-compatibility requirement.
</action>
  <verify>
`python -m scripts.cli --help` outputs Click help with `template` subcommand listed
</verify>
  <done>CLI group responds with help, template subcommand registered</done>
</task>

<task type="auto">
  <name>Task 2: Implement template generator with smoke/perf/longrun templates</name>
  <files>scripts/cli/template.py, scripts/cli/templates/smoke.yaml, scripts/cli/templates/perf.yaml, scripts/cli/templates/longrun.yaml</files>
  <action>
Create the template generator module and template YAML files.

**scripts/cli/template.py:**
- `TEMPLATE_DIR = Path(__file__).parent / "templates"`
- `def load_template(template_type: str) -> dict:` — loads YAML from `templates/{type}.yaml`
- `def apply_overrides(template: dict, op: str, threads: int, object_size: str) -> dict:` — creates scenario config from template + user overrides
- `def generate_config(op, threads, object_size, template_type) -> dict:` — main entry point
- `def explain_config(config: dict, template_type: str, op: str, threads: int, object_size: str) -> str:` — returns 5-column Markdown table

**5-column explain table format (per D-03):**
```
| 字段 | 来源 | 值 | 说明 | 生效状态 |
|------|------|-----|------|---------|
| threads | CLI | 128 | 用户指定 | ✓ |
| object_size | default | 1MB | smoke模板默认值 | ✓ |
```

**scripts/cli/templates/smoke.yaml:**
```yaml
# Fast validation: 5 requests, 2 threads, 1MB
suite_id: smoke_template
defaults:
  users_file: ./users.dat
  requests_per_thread: 5
profiles:
  base:
    config_file: ./config.dat
scenarios:
  - scenario_id: smoke_upload_1mb_2t
    profile: base
    op: upload
    threads: 2
    object_size: 1MB
```

**scripts/cli/templates/perf.yaml:**
```yaml
# Performance benchmark: 100 requests, 64-128 threads
suite_id: perf_template
defaults:
  users_file: ./users.dat
  requests_per_thread: 100
profiles:
  base:
    config_file: ./config.dat
scenarios:
  - scenario_id: perf_upload_1mb_128t
    profile: base
    op: upload
    threads: 128
    object_size: 1MB
```

**scripts/cli/templates/longrun.yaml:**
```yaml
# Long-run stability: 3600s, 64 threads, 4MB
suite_id: longrun_template
defaults:
  users_file: ./users.dat
  run_seconds: 3600
profiles:
  base:
    config_file: ./config.dat
scenarios:
  - scenario_id: longrun_upload_4mb_64t
    profile: base
    op: upload
    threads: 64
    object_size: 4MB
    analyze_longrun: true
```

Template files use the EXACT same structure as `ci/perf/suites/examples/smoke_check.yaml` to ensure backward compatibility with `resolve_suite.py`.
</action>
  <verify>
`python -c "from scripts.cli.template import generate_config, explain_config; c = generate_config('upload', 128, '1MB', 'smoke'); print(explain_config(c, 'smoke', 'upload', 128, '1MB'))"` outputs 5-column Markdown table
</verify>
  <done>Template generator produces valid YAML compatible with resolve_suite.py parsing</done>
</task>

<task type="auto">
  <name>Task 3: Add parameter validation and defaults resolution</name>
  <files>scripts/cli/validators.py, scripts/cli/defaults.py</files>
  <action>
Create validators and defaults resolution modules.

**scripts/cli/validators.py:**
```python
VALID_OPS = {'upload', 'download'}

def validate_params(op: str, threads: int, object_size: str) -> list[str]:
    """Returns list of error messages (empty = valid)."""
    errors = []
    if op not in VALID_OPS:
        errors.append(f"Invalid op '{op}'. Must be one of: {', '.join(sorted(VALID_OPS))}")
    if threads <= 0:
        errors.append(f"threads must be > 0, got {threads}")
    if not object_size or object_size == '0':
        errors.append(f"object_size must be non-empty and > 0, got '{object_size}'")
    return errors
```

**scripts/cli/defaults.py:**
- Implement `resolve_effective_config(scenario: dict, defaults: dict) -> dict` following the pattern from `resolve_suite.py` `materialize_scenario()`
- Priority: scenario explicit value > defaults > hard-coded defaults
- Extend the existing merge logic from `resolve_suite.py`:
  - Import `merge_dicts` from `scripts.suites.resolve_suite`
  - Use same field normalization: `normalize_op()`, `clean_text()`
  - Handle `object_size` vs `object_size_spec` alias

The defaults resolution must be compatible with `resolve_suite.py` parsing so that generated YAML can be fed back through the suite resolver.

Integration: Modify `scripts/cli/template.py` to call `validate_params()` before generating, and `resolve_effective_config()` to merge scenario with template defaults.
</action>
  <verify>
`python -c "from scripts.cli.validators import validate_params; print(validate_params('invalid_op', 0, ''))"` outputs 3 error messages; `python -c "from scripts.cli.validators import validate_params; print(validate_params('upload', 128, '1MB'))"` outputs empty list
</verify>
  <done>Validation catches invalid parameters with clear error messages; defaults resolution produces merged config with correct priority</done>
</task>

</tasks>

<threat_model>
## Trust Boundaries

| Boundary | Description |
|----------|-------------|
| user input -> CLI | Unvalidated string input crosses here (op, threads, object_size) |

## STRIDE Threat Register

| Threat ID | Category | Component | Disposition | Mitigation Plan |
|-----------|----------|-----------|-------------|-----------------|
| T-01-01 | Tamper | Generated YAML | mitigate | Validate all inputs before YAML generation; use yaml.safe_dump (no arbitrary code execution) |
| T-01-02 | Information Disclosure | Template file paths | accept | Template paths are internal; user specifies op/threads/size only |
| T-01-03 | Denial of Service | Large object_size string | mitigate | Validate object_size format with regex before passing to template |
</threat_model>

<verification>
- `python -m scripts.cli template --help` shows all options
- `python -m scripts.cli template --op upload --threads 128 --object-size 1MB --output /tmp/test.yaml` generates valid YAML
- `python -m scripts.cli template --op upload --threads 128 --object-size 1MB --explain` shows 5-column provenance table
- `python -m scripts.cli template --op invalid --threads 0 --object-size ""` shows 3 validation errors
- Generated YAML parses successfully through `resolve_suite.py`
</verification>

<success_criteria>
1. `python -m scripts.cli template --op upload --threads 128 --object-size 1MB --output scenario.yaml` generates scenario YAML with all required fields
2. `python -m scripts.cli template --explain` outputs 5-column Markdown table showing field provenance
3. `python -m scripts.cli template --wizard` enters interactive mode (per D-01)
4. Validation errors appear for: invalid op, threads <= 0, empty object_size
5. Generated YAML is backward-compatible with existing `resolve_suite.py` parsing
6. All three templates (smoke/perf/longrun) produce valid scenario YAML
</success_criteria>

<output>
After completion, create `.planning/phases/01-configuration-simplification/01-SUMMARY.md`
</output>
