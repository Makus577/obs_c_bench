#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OBS C Benchmark Tool - Simplified workflow orchestration.

Entry point: python -m scripts.cli [command]
Or (if installed): python -m obs_bench.cli [command]
"""

import click

from scripts.cli.template import generate_config, explain_config


@click.group()
def cli():
    """OBS C Benchmark Tool - Simplified workflow orchestration"""
    pass


@cli.command()
@click.option('--config', 'config_path', type=click.Path(exists=True),
              help='Path to scenario YAML (mutually exclusive with --suite)')
@click.option('--suite', 'suite_path', type=click.Path(exists=True),
              help='Path to suite YAML (mutually exclusive with --config)')
@click.option('--output-dir', 'output_dir', type=click.Path(),
              default='./reports', help='Output directory for all reports')
@click.option('--run-id', 'run_id', type=str, default=None,
              help='Custom run identifier (default: auto-generated from scenario/suite + timestamp)')
@click.option('--skip-plot', is_flag=True,
              help='Skip the dashboard plotting step (useful when debugging gate failures)')
@click.option('--dry-run', is_flag=True,
              help='Validate configuration without executing the benchmark')
@click.option('--benchmark-binary', 'benchmark_binary', type=click.Path(exists=True),
              default=None, help='Path to obs_c_bench binary (default: auto-detect)')
def run(config_path, suite_path, output_dir, run_id, skip_plot, dry_run, benchmark_binary):
    """Execute the full benchmarking pipeline with a command.

    Chains: benchmark -> merge_details -> analyze_longrun -> perf_gate -> plot_report

    Examples:

      python -m obs_bench.cli run --config scenario.yaml --output-dir ./reports

      python -m obs_bench.cli run --suite suite.yaml --output-dir ./reports

      python -m obs_bench.cli run --config scenario.yaml --skip-plot --output-dir ./reports

      python -m obs_bench.cli run --config scenario.yaml --dry-run
    """
    from scripts.orchestration.run_benchmark import run as orchestrate
    from scripts.orchestration.dry_run import dry_run_validate

    if dry_run:
        dry_run_validate(config_path, suite_path)
        click.echo("[+] Configuration is valid.")
        return

    if not config_path and not suite_path:
        click.echo("ERROR: Must provide either --config or --suite", err=True)
        raise SystemExit(1)

    if config_path and suite_path:
        click.echo("ERROR: --config and --suite are mutually exclusive", err=True)
        raise SystemExit(1)

    exit_code = orchestrate(
        config_path=config_path,
        suite_path=suite_path,
        output_dir=output_dir,
        run_id=run_id,
        skip_plot=skip_plot,
        benchmark_binary=benchmark_binary,
    )
    raise SystemExit(exit_code)


@cli.command()
@click.option('--op', 'op', required=True,
              type=click.Choice(['upload', 'download']),
              help='Operation type')
@click.option('--threads', type=int, required=True,
              help='Number of threads')
@click.option('--object-size', 'object_size', required=True,
              help='Object size (e.g., 1MB, 4MB)')
@click.option('--template', 'template_type',
              type=click.Choice(['smoke', 'perf', 'longrun']),
              default='smoke',
              help='Template type (default: smoke)')
@click.option('--output', '-o', type=click.Path(),
              help='Output file (default: stdout)')
@click.option('--explain', is_flag=True,
              help='Show field provenance table (5-column format)')
@click.option('--wizard', is_flag=True,
              help='Interactive wizard mode')
def template(op, threads, object_size, template_type, output, explain, wizard):
    """Generate a scenario YAML from minimal input.

    This command generates a valid scenario YAML configuration file from
    just three required parameters: operation type, thread count, and
    object size.

    Examples:

      python -m scripts.cli template --op upload --threads 128 --object-size 1MB

      python -m scripts.cli template --op download --threads 64 --object-size 4MB \\
          --template longrun --output longrun.yaml

      python -m scripts.cli template --op upload --threads 128 --object-size 1MB \\
          --explain
    """
    if wizard:
        # Wizard mode: interactive prompting for all options
        _run_wizard()
        return

    from scripts.cli.validators import validate_params

    errors = validate_params(op, threads, object_size)
    if errors:
        for err in errors:
            click.echo(f"ERROR: {err}", err=True)
        raise SystemExit(1)

    try:
        config = generate_config(op, threads, object_size, template_type)
    except Exception as exc:
        click.echo(f"ERROR: Failed to generate config: {exc}", err=True)
        raise SystemExit(1)

    if explain:
        table = explain_config(config, template_type, op, threads, object_size)
        click.echo(table)
        click.echo()

    import yaml
    yaml_output = yaml.safe_dump(config, allow_unicode=True, sort_keys=False, default_flow_style=False)

    if output:
        import os
        os.makedirs(os.path.dirname(os.path.abspath(output)) if os.path.dirname(output) else '.', exist_ok=True)
        with open(output, 'w', encoding='utf-8') as f:
            f.write(yaml_output)
        click.echo(f"Generated: {output}")
    else:
        click.echo(yaml_output)


def _run_wizard():
    """Interactive wizard mode."""
    click.echo("OBS C Benchmark - Configuration Wizard")
    click.echo("=" * 40)

    op = click.prompt('Operation', type=click.Choice(['upload', 'download']))
    threads = click.prompt('Number of threads', type=int)
    object_size = click.prompt('Object size (e.g., 1MB, 4MB)', type=str)
    template_type = click.prompt('Template type', type=click.Choice(['smoke', 'perf', 'longrun']), default='smoke')
    explain = click.confirm('Show field provenance table?')
    output = click.prompt('Output file (leave empty for stdout)', type=str, default='')

    errors = validate_params(op, threads, object_size)
    if errors:
        for err in errors:
            click.echo(f"ERROR: {err}", err=True)
        raise SystemExit(1)

    try:
        config = generate_config(op, threads, object_size, template_type)
    except Exception as exc:
        click.echo(f"ERROR: Failed to generate config: {exc}", err=True)
        raise SystemExit(1)

    if explain:
        table = explain_config(config, template_type, op, threads, object_size)
        click.echo(table)
        click.echo()

    import yaml
    yaml_output = yaml.safe_dump(config, allow_unicode=True, sort_keys=False, default_flow_style=False)

    if output:
        import os
        os.makedirs(os.path.dirname(os.path.abspath(output)) if os.path.dirname(output) else '.', exist_ok=True)
        with open(output, 'w', encoding='utf-8') as f:
            f.write(yaml_output)
        click.echo(f"Generated: {output}")
    else:
        click.echo(yaml_output)


def validate_params(op, threads, object_size):
    """Validate parameters. Imported here to avoid circular imports in wizard."""
    from scripts.cli.validators import validate_params as _vp
    return _vp(op, threads, object_size)


def main():
    """Entry point when run as: python -m scripts.cli"""
    cli()


if __name__ == '__main__':
    main()
