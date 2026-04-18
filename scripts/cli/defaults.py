#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Defaults resolution engine for scenario YAML.

Merges scenario configurations with template defaults using the same
priority rules as resolve_suite.py:
  scenario explicit value > defaults > hard-coded defaults

This module is imported and extended from scripts/suites/resolve_suite.py
to ensure backward compatibility with the suite resolver.
"""

from typing import Any, Dict

from scripts.suites.resolve_suite import merge_dicts


# Hard-coded fallback defaults when no template or CLI value is provided
HARD_CODED_DEFAULTS = {
    'requests_per_thread': 100,
    'run_seconds': 300,
    'object_size': '1MB',
    'threads': 64,
}


def normalize_op(value: Any) -> str:
    """Normalize operation value to lowercase string."""
    if value is None:
        return ''
    return str(value).strip().lower()


def clean_text(value: Any) -> str:
    """Clean and normalize text value."""
    if value is None:
        return ''
    return str(value).strip()


def resolve_effective_config(
    scenario: Dict[str, Any],
    defaults: Dict[str, Any]
) -> Dict[str, Any]:
    """Resolve effective configuration by merging scenario with defaults.

    Priority (highest to lowest):
      1. Scenario explicit value (non-empty/non-None)
      2. Defaults block value (non-empty/non-None)
      3. Hard-coded fallback defaults

    This follows the same merge pattern as materialize_scenario() in
    resolve_suite.py but operates on a single scenario dict.

    Args:
        scenario: Scenario-specific configuration dict
        defaults: Defaults block from template

    Returns:
        Merged configuration dict with all fields resolved
    """
    # Start with hard-coded defaults
    result = HARD_CODED_DEFAULTS.copy()

    # Merge defaults (second priority)
    if defaults:
        result = merge_dicts(result, defaults)

    # Merge scenario (highest priority)
    if scenario:
        result = merge_dicts(result, scenario)

    # Normalize specific fields
    result['op'] = normalize_op(result.get('op'))

    # Handle object_size vs object_size_spec alias
    if 'object_size_spec' in result and 'object_size' not in result:
        result['object_size'] = clean_text(result.pop('object_size_spec'))
    else:
        result['object_size'] = clean_text(result.get('object_size', ''))

    # Ensure primitive fields are properly typed
    if 'threads' in result:
        try:
            result['threads'] = int(result['threads'])
        except (ValueError, TypeError):
            result['threads'] = HARD_CODED_DEFAULTS['threads']

    if 'run_seconds' in result and result['run_seconds']:
        try:
            result['run_seconds'] = int(result['run_seconds'])
        except (ValueError, TypeError):
            result['run_seconds'] = ''

    if 'requests_per_thread' in result and result['requests_per_thread']:
        try:
            result['requests_per_thread'] = int(result['requests_per_thread'])
        except (ValueError, TypeError):
            result['requests_per_thread'] = ''

    return result
