# Stack Research

**Domain:** Benchmark Tooling Enhancement (C-based OBS benchmark with Python reporting)
**Researched:** 2026-04-19
**Confidence:** MEDIUM

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| **Click** | 8.x | CLI framework for workflow orchestration | Standard Python CLI toolkit; decorators provide clean command grouping; supports shell completion natively; better for complex multi-command CLIs than argparse |
| **PyYAML** | 6.x | YAML configuration parsing | Already in project dependencies; `safe_load` prevents arbitrary code execution; LibYAML C extension available for performance |
| **Python 3.8+** | 3.8+ | Script runtime | Project already requires Python 3.x; `typing.Annotated` available in 3.9+ but can use `typing_extensions` for earlier versions |

### Supporting Libraries

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| pandas | latest | Data manipulation for report generation | Already in project; use for `archive.csv` analysis, longrun metrics aggregation |
| matplotlib | latest | Visualization | Already in project; use for dashboard generation |
| numpy | latest | Numerical analysis | Already in project; use for statistical calculations (slopes, percentiles) |
| typing_extensions | 4.x | Backport for `Annotated` types | When targeting Python 3.8 compatibility with modern type hints |

### Workflow Orchestration Pattern

**Recommended: Click-based unified CLI with subcommands**

```
obs_bench.py run      # Execute benchmark (wraps obs_c_bench)
obs_bench.py analyze  # Analyze results (merge_details + analyze_longrun + plot_report)
obs_bench.py compare  # Compare with baseline (perf_gate)
obs_bench.py template # Generate simplified configuration
obs_bench.py chain    # One-click full pipeline
```

### CLI Design Patterns

**Pattern 1: Command Groups with Subcommands**

```python
import click
import subprocess
from pathlib import Path

@click.group()
def cli():
    """OBS C Benchmark Tool - Simplified workflow orchestration"""
    pass

@cli.command()
@click.option('--config', type=click.Path(exists=True), help='Scenario YAML path')
@click.option('--threads', type=int, help='Override thread count')
@click.option('--op', type=click.Choice(['upload', 'download']), help='Operation type')
def run(config, threads, op):
    """Execute benchmark test"""
    cmd = ['./obs_c_bench']
    if config:
        cmd.extend(['--config', config])
    if threads:
        cmd.extend(['--threads', str(threads)])
    if op:
        cmd.extend(['--op', op])
    subprocess.run(cmd)

@cli.command()
@click.option('--report-dir', type=click.Path(), help='Report directory')
def analyze(report_dir):
    """Run full analysis pipeline"""
    # merge_details → analyze_longrun → plot_report
    pass
```

**Pattern 2: Intelligent Defaults with Progressive Disclosure**

```python
# Template generator that creates minimal configs
DEFAULT_VALUES = {
    'threads': 64,
    'object_size': '1MB',
    'run_seconds': 300,
    'requests_per_thread': 0,
}

SIMPLE_FIELDS = ['op', 'threads', 'object_size', 'scenario_id']
ADVANCED_FIELDS = ['profile', 'users_file', 'range', 'part_size']

def generate_simple_config(op, threads=None, object_size=None):
    """Generate simplified scenario.yaml with smart defaults"""
    config = {
        'profile': './config.dat',
        'users': './users.dat',
        'op': op,
        'threads': threads or DEFAULT_VALUES['threads'],
        'object_size': object_size or DEFAULT_VALUES['object_size'],
        'scenario_id': f'{op}_{object_size or "1mb"}_{threads or 64}t',
    }
    return config
```

**Pattern 3: One-Click Chain with Error Handling**

```python
@cli.command()
@click.option('--config', type=click.Path(exists=True))
@click.option('--compare/--no-compare', default=False)
@click.option('--output-dir', type=click.Path(), default='./reports')
def chain(config, compare, output_dir):
    """Execute: run → merge → analyze → compare → plot"""
    report_dir = run_benchmark(config)
    merge_result = merge_details(report_dir)
    longrun_result = analyze_longrun(report_dir)
    if compare:
        compare_result = perf_gate(report_dir)
    plot_report(report_dir)
    click.echo(f"Pipeline complete. Reports in: {output_dir}")
```

### Configuration Simplification Patterns

**Pattern 4: Template-Based Generation**

```python
TEMPLATES = {
    'upload_smoke': {
        'op': 'upload',
        'threads': 32,
        'object_size': '1MB',
        'run_seconds': 60,
    },
    'upload_perf': {
        'op': 'upload',
        'threads': 128,
        'object_size': '1MB',
        'run_seconds': 300,
    },
    'download_perf': {
        'op': 'download',
        'threads': 128,
        'object_size': '4MB',
        'run_seconds': 300,
    },
}

def apply_template(template_name, customizations=None):
    """Apply template with optional overrides"""
    template = TEMPLATES.get(template_name, TEMPLATES['upload_smoke'])
    if customizations:
        template.update(customizations)
    return template
```

**Pattern 5: Intelligent Parameter Inheritance**

```python
def resolve_effective_config(scenario_yaml, cli_overrides):
    """Merge config with priority: CLI > scenario.yaml > defaults"""
    import yaml
    with open(scenario_yaml) as f:
        base = yaml.safe_load(f)

    # Apply CLI overrides
    effective = {**base, **cli_overrides}

    # Smart defaults for missing fields
    effective.setdefault('threads', 64)
    effective.setdefault('object_size', '1MB')
    effective.setdefault('run_seconds', 300)

    return effective
```

### Integration with Existing Scripts

**Key Principle: Wrap existing scripts, don't rewrite them**

```python
# Wrapper that maintains compatibility with existing scripts
WRAPPER_SCRIPTS = {
    'merge_details': 'scripts/reporting/merge_details.py',
    'analyze_longrun': 'scripts/reporting/analyze_longrun.py',
    'perf_gate': 'scripts/reporting/perf_gate.py',
    'plot_report': 'scripts/reporting/plot_report.py',
}

def invoke_script(script_name, args, cwd=None):
    """Invoke existing script with arguments"""
    script_path = Path(REPO_ROOT) / WRAPPER_SCRIPTS[script_name]
    result = subprocess.run(
        ['python3', str(script_path)] + args,
        cwd=cwd or REPO_ROOT,
        capture_output=True,
        text=True
    )
    return result
```

## Alternatives Considered

| Recommended | Alternative | When to Use Alternative |
|-------------|-------------|------------------------|
| Click | Typer | Typer is simpler but less battle-tested for complex CLI with shell completion |
| Click | argparse (existing) | argparse is fine for simple scripts but becomes unwieldy with 5+ subcommands |
| Click | argparse + argcomplete | If avoiding new dependencies, argparse+argcomplete provides similar features |
| Wrapper approach | Rewrite in Click | Rewriting existing scripts risks breaking compatibility; wrapper preserves existing behavior |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| argparse directly | Verbose for complex CLI; manual help formatting | Click decorators |
| raw `yaml.load()` | Security risk (arbitrary code execution) | `yaml.safe_load()` |
| os.system() | No error handling; shell injection risk | subprocess.run() |
| Hardcoded paths | Not portable | `pathlib.Path` with `__file__` based resolution |

## Stack Patterns by Variant

**If Python 3.8 compatibility required:**
- Use `typing_extensions.Annotated` instead of `typing.Annotated`
- Use `click.Argument` with `nargs=-1` for variadic arguments

**If Python 3.9+ only:**
- Use native `typing.Annotated`
- More readable type hints

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| Click 8.x | Python 3.7+ | Current standard; LTS maintenance |
| PyYAML 6.x | Python 3.6+ | Use `yaml.safe_load()` explicitly |
| pandas | Python 3.8+ | Already in project dependencies |
| matplotlib | Python 3.8+ | Already in project dependencies |
| numpy | Python 3.8+ | Already in project dependencies |

## Sources

- [Click Context7](/pallets/click) — CLI command patterns, option decorators, command groups
- [Typer Context7](/fastapi/typer) — Type-hint based CLI design
- [PyYAML Context7](/yaml/pyyaml) — Safe YAML loading, LibYAML performance extension
- [Click Documentation](https://click.palletsdocs.com/) — Best practices for CLI design (via WebFetch)
- Project existing scripts — Current argparse patterns, workflow integration points

---
*Stack research for: Benchmark Tooling Enhancement*
*Researched: 2026-04-19*
