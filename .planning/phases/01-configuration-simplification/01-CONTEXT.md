# Phase 1: Configuration Simplification - Context

**Gathered:** 2026-04-19
**Status:** Ready for planning

<domain>
## Phase Boundary

Users can generate valid scenario configurations with minimal input. This phase delivers:
1. **Template generator CLI** — `python -m obs_bench.cli template` with smoke/perf/longrun templates
2. **Smart defaults + inheritance** — `defaults:` block in Suite YAML with scenario override support

</domain>

<decisions>
## Implementation Decisions

### CLI Style (REQ-01)
- **D-01:** Hybrid mode — default single-command, `--wizard` flag enables interactive mode
- Single-command example: `python -m obs_bench.cli template --op upload --threads 128 --object-size 1MB --output my_scenario.yaml`
- Wizard mode: `python -m obs_bench.cli template --wizard` guides user through questions

### Template Types (REQ-01)
- **D-02:** Three preset templates: `smoke` (快速验证), `perf` (性能测试), `longrun` (长稳测试)
- Each template has sensible defaults for its use case:
  - `smoke`: 5 requests, 2 threads, 1MB — fast validation
  - `perf`: 100 requests, 64-128 threads, various sizes — standard benchmark
  - `longrun`: 3600s runtime, 64 threads, 4MB — long stability observation

### Explain Output Format (REQ-01)
- **D-03:** 5-column Markdown table: 字段 | 来源 | 值 | 说明 | 生效状态
- Example:
  | 字段 | 来源 | 值 | 说明 | 生效 |
  |------|------|-----|------|------|
  | threads | CLI | 128 | 用户指定 | ✓ |
  | object_size | default | 1MB | smoke模板默认值 | ✓ |
  | run_seconds | default | 5 | smoke模板默认值 | - |

### Defaults Inheritance (REQ-04)
- **D-04:** scenario 显式值 overrides defaults — scenario 优先级更高
- Priority order: scenario explicit > defaults > hard-coded defaults
- Validation error if neither scenario nor defaults provides required field

### CLI Entry Point
- **D-05:** CLI module path: `scripts/cli/` as `python -m obs_bench.cli`
- Internal module structure: `scripts/orchestration/` for workflow, `scripts/templates/` for template files
- Backward compatible: existing `scripts/reporting/*.py` remain independently runnable

### Validation
- **D-06:** Parameter validation before YAML generation (threads > 0, object_size > 0, valid op type)
- Clear error messages pointing to which parameter failed

### Folded Todos
(None — no backlog items matched this phase)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Existing Templates
- `ci/perf/suites/examples/smoke_check.yaml` — smoke template reference
- `ci/perf/suites/examples/longrun_test.yaml` — longrun template reference

### Existing Scripts
- `scripts/reporting/analyze_longrun.py` — uses argparse, reference for CLI patterns
- `scripts/suites/resolve_suite.py` — suite YAML parsing, reference for config loading

### Project Requirements
- `.planning/REQUIREMENTS.md` — REQ-01, REQ-04 acceptance criteria
- `.planning/ROADMAP.md` — Phase 1 success criteria

### Stack Research
- `.planning/research/STACK.md` — Click 8.x recommended for CLI framework
- `.planning/research/ARCHITECTURE.md` — subprocess wrapper pattern, backward compatibility requirement

</canonical_refs>

<codebase_context>
## Existing Code Insights

### Reusable Assets
- `scripts/reporting/analyze_longrun.py` — argparse CLI pattern to follow
- `ci/perf/suites/examples/*.yaml` — existing template structure to extend

### Established Patterns
- Suite YAML uses `defaults:`, `profiles:`, `scenarios:`, `baseline:`, `gates:` blocks
- Config loading in `src/config_loader.c` — validation logic should mirror existing

### Integration Points
- Template generator reads from `scripts/templates/<type>.yaml`
- Generated YAML must be compatible with existing `config_loader.c` parsing
- CLI module at `scripts/cli/` wraps existing scripts via subprocess

</codebase_context>

<specifics>
## Specific Ideas

- Template generator `--explain` must show which fields are auto-filled and why
- Generated scenario.yaml must include `profile: ./config.dat` (or inherited profile)
- Wizard mode asks only: op, threads, object_size, template_type, output_path

</specifics>

<deferred>
## Deferred Ideas

### Reviewed Todos (not folded)
(None — no unmatched todos)

</deferred>

---

*Phase: 01-configuration-simplification*
*Context gathered: 2026-04-19*
