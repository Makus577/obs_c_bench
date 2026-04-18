#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Dry-run validation: verify scenario/suite YAMLs are parseable without executing."""

import os
import yaml


def dry_run_validate(config_path: str, suite_path: str) -> None:
    """Validate scenario/suite YAMLs are parseable without executing the benchmark.

    Raises ValueError with descriptive message on any validation failure.
    """
    errors = []

    if config_path:
        try:
            with open(config_path, 'r', encoding='utf-8') as f:
                doc = yaml.safe_load(f)
            if not isinstance(doc, dict):
                errors.append(f"Scenario YAML root must be a mapping, got {type(doc).__name__}")
            required = ['profile']
            for field in required:
                if field not in doc:
                    errors.append(f"Scenario YAML missing required field: '{field}'")
        except yaml.YAMLError as e:
            errors.append(f"Scenario YAML parse error: {e}")
        except FileNotFoundError:
            errors.append(f"Scenario file not found: {config_path}")

    if suite_path:
        try:
            from scripts.suites.resolve_suite import (
                build_default_settings,
                build_profile_map,
                expand_matrix,
                load_suite,
                materialize_scenario,
            )
            doc = load_suite(suite_path)
            suite_dir = os.path.dirname(os.path.abspath(suite_path))
            defaults = build_default_settings(doc, suite_dir)
            profiles = build_profile_map(doc, suite_dir)
            scenarios = expand_matrix(
                "dry_run", suite_dir, defaults, profiles,
                doc.get("matrix") or {}
            )
            for raw in doc.get("scenarios") or []:
                scenarios.append(materialize_scenario(
                    "dry_run", suite_dir, defaults, profiles, raw, ""
                ))
            if not scenarios:
                errors.append("Suite YAML produced no enabled scenarios")
        except yaml.YAMLError as e:
            errors.append(f"Suite YAML parse error: {e}")
        except FileNotFoundError:
            errors.append(f"Suite file not found: {suite_path}")
        except Exception as e:
            errors.append(f"Suite validation error: {e}")

    if errors:
        raise ValueError("Dry-run validation failed:\n" + "\n".join(f"  - {err}" for err in errors))
