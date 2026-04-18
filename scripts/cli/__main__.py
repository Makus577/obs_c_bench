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
