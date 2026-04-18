#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Template generator for scenario YAML files.

Provides functions to load templates, apply user overrides, and generate
explain tables showing field provenance.
"""

import os
import re
from pathlib import Path
from typing import Any, Dict

try:
    import yaml
except ImportError:  # pragma: no cover - runtime guard
    raise SystemExit("PyYAML is required for template generation. Install with `pip install PyYAML`.")


TEMPLATE_DIR = Path(__file__).parent / "templates"


def load_template(template_type: str) -> Dict[str, Any]:
    """Load a template YAML file by type.

    Args:
        template_type: One of 'smoke', 'perf', 'longrun'

    Returns:
        Parsed YAML template as a dict

    Raises:
        FileNotFoundError: If template file does not exist
        ValueError: If template_type is unknown
    """
    if template_type not in ('smoke', 'perf', 'longrun'):
        raise ValueError(f"Unknown template type '{template_type}'. Must be one of: smoke, perf, longrun")

    template_path = TEMPLATE_DIR / f"{template_type}.yaml"
    if not template_path.exists():
        raise FileNotFoundError(f"Template not found: {template_path}")

    with open(template_path, "r", encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def apply_overrides(
    template: Dict[str, Any],
    op: str,
    threads: int,
    object_size: str
) -> Dict[str, Any]:
    """Apply user overrides to a template and return a complete scenario config.

    Args:
        template: The loaded template dict
        op: User-specified operation ('upload' or 'download')
        threads: User-specified thread count
        object_size: User-specified object size (e.g., '1MB', '4MB')

    Returns:
        Complete scenario config dict ready for YAML serialization
    """
    import copy
    config = copy.deepcopy(template)

    # Update scenario parameters with user overrides
    if 'scenarios' in config and len(config['scenarios']) > 0:
        scenario = config['scenarios'][0]
        scenario['op'] = op
        scenario['threads'] = threads
        scenario['object_size'] = object_size

        # Auto-generate scenario_id if not provided
        object_size_slug = object_size.lower().replace(' ', '')
        scenario['scenario_id'] = f"{template.get('suite_id', 'scenario')}_{op}_{object_size_slug}_{threads}t"

    return config


def generate_config(
    op: str,
    threads: int,
    object_size: str,
    template_type: str = 'smoke'
) -> Dict[str, Any]:
    """Generate a scenario YAML configuration from minimal user input.

    Args:
        op: Operation type ('upload' or 'download')
        threads: Number of threads
        object_size: Object size specification (e.g., '1MB', '4MB')
        template_type: Template to use ('smoke', 'perf', 'longrun')

    Returns:
        Complete scenario configuration dict
    """
    from scripts.cli.validators import validate_params

    # Validate parameters first
    errors = validate_params(op, threads, object_size)
    if errors:
        raise ValueError("; ".join(errors))

    # Load template and apply overrides
    template = load_template(template_type)
    config = apply_overrides(template, op, threads, object_size)

    return config


def explain_config(
    config: Dict[str, Any],
    template_type: str,
    op: str,
    threads: int,
    object_size: str
) -> str:
    """Generate a 5-column Markdown provenance table.

    Columns: 字段 | 来源 | 值 | 说明 | 生效状态

    Args:
        config: The resolved configuration dict
        template_type: Which template was used
        op: User-specified operation
        threads: User-specified thread count
        object_size: User-specified object size

    Returns:
        Markdown-formatted provenance table as a string
    """
    lines = []
    lines.append("| 字段 | 来源 | 值 | 说明 | 生效状态 |")
    lines.append("|------|------|-----|------|---------|")

    # Template fields with their sources
    field_sources = [
        ('suite_id', 'template', config.get('suite_id', ''), 'Suite identifier', '✓'),
    ]

    # Defaults section
    defaults = config.get('defaults', {})
    for field in ['users_file', 'requests_per_thread', 'run_seconds']:
        value = defaults.get(field, '')
        if value:
            source_desc = f"default (from {template_type} template)"
            field_name = field.replace('_', ' ').title().replace(' ', '_')
            lines.append(f"| {field} | {source_desc} | {value} | Template default | ✓ |")

    # Scenario fields
    scenarios = config.get('scenarios', [])
    if scenarios:
        scenario = scenarios[0]

        # Fields explicitly set by user
        lines.append(f"| op | CLI | {op} | 用户指定 | ✓ |")
        lines.append(f"| threads | CLI | {threads} | 用户指定 | ✓ |")
        lines.append(f"| object_size | CLI | {object_size} | 用户指定 | ✓ |")

        # scenario_id is auto-generated
        lines.append(f"| scenario_id | auto | {scenario.get('scenario_id', '')} | 自动生成 | ✓ |")

        # Profile from template
        profile = scenario.get('profile', '')
        lines.append(f"| profile | template | {profile} | 继承自模板 | ✓ |")

        # Config file from template
        profile_data = config.get('profiles', {}).get(profile, {})
        config_file = profile_data.get('config_file', '')
        lines.append(f"| config_file | template | {config_file} | 继承自模板 | ✓ |")

    return "\n".join(lines)
