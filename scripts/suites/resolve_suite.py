#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import copy
import itertools
import os
import re
import sys
from typing import Any, Dict, List

try:
    import yaml  # type: ignore
except ImportError as exc:  # pragma: no cover - runtime guard
    raise SystemExit("PyYAML is required for suite mode. Install it with `pip install PyYAML`.") from exc


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_LONGRUN_POLICY = os.path.join(REPO_ROOT, "ci", "perf", "longrun_policy.yaml")
DEFAULT_PERF_POLICY = os.path.join(REPO_ROOT, "ci", "perf", "gate_policy.json")
DEFAULT_BASELINES_MANIFEST = os.path.join(REPO_ROOT, "ci", "perf", "baselines.csv")
DEFAULT_BASELINE_STORE = os.path.join(REPO_ROOT, "ci", "perf", "baseline_archives")


def clean_text(value: Any) -> str:
    if value is None:
        return ""
    return str(value).strip()


def slugify(value: Any) -> str:
    text = clean_text(value).lower().replace("~", "_to_")
    text = re.sub(r"[^a-z0-9]+", "_", text)
    return text.strip("_") or "unknown"


def ensure_directory(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def resolve_path(base_dir: str, value: Any) -> str:
    text = clean_text(value)
    if not text:
        return ""
    if os.path.isabs(text):
        return text
    return os.path.abspath(os.path.join(base_dir, text))


def parse_bool(value: Any, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    text = clean_text(value).lower()
    if text in ("1", "true", "yes", "on"):
        return True
    if text in ("0", "false", "no", "off"):
        return False
    return default


def listify(value: Any) -> List[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def merge_dicts(*parts: Dict[str, Any]) -> Dict[str, Any]:
    merged: Dict[str, Any] = {}
    for part in parts:
        if not isinstance(part, dict):
            continue
        for key, value in part.items():
            merged[key] = copy.deepcopy(value)
    return merged


def normalize_op(value: Any) -> str:
    text = clean_text(value)
    if not text:
        return ""
    return text


def build_default_settings(doc: Dict[str, Any], suite_dir: str) -> Dict[str, Any]:
    defaults = merge_dicts(doc.get("defaults") or {})
    reporting = doc.get("reporting") or {}
    gates = doc.get("gates") or {}
    if "perf" in gates:
        raise ValueError("gates.perf is no longer supported. Use top-level baseline.* instead.")
    longrun = gates.get("longrun") or {}
    baseline = doc.get("baseline") or {}

    defaults["users_file"] = resolve_path(suite_dir, defaults.get("users_file"))
    defaults["allow_open_ended_run"] = parse_bool(defaults.get("allow_open_ended_run"), False)
    defaults["continue_on_fail"] = parse_bool(
        reporting.get("continue_on_fail", defaults.get("continue_on_fail")),
        True,
    )
    defaults["analyze_longrun"] = parse_bool(
        longrun.get("enabled", defaults.get("analyze_longrun")),
        False,
    )
    defaults["gate_longrun"] = parse_bool(
        longrun.get("fail_on_regression", defaults.get("gate_longrun")),
        False,
    )
    defaults["longrun_policy"] = resolve_path(
        suite_dir,
        longrun.get("policy") or defaults.get("longrun_policy") or DEFAULT_LONGRUN_POLICY,
    )
    defaults["baseline_mode"] = clean_text(baseline.get("mode") or defaults.get("baseline_mode") or "off").lower()
    defaults["baseline_enabled"] = parse_bool(
        baseline.get("enabled", defaults.get("baseline_enabled")),
        defaults["baseline_mode"] in ("generate", "compare"),
    )
    defaults["baseline_policy"] = resolve_path(
        suite_dir,
        baseline.get("policy") or defaults.get("baseline_policy") or DEFAULT_PERF_POLICY,
    )
    defaults["baseline_manifest"] = resolve_path(
        suite_dir,
        baseline.get("manifest") or defaults.get("baseline_manifest") or DEFAULT_BASELINES_MANIFEST,
    )
    defaults["baseline_store_dir"] = resolve_path(
        suite_dir,
        baseline.get("store_dir") or defaults.get("baseline_store_dir") or DEFAULT_BASELINE_STORE,
    )
    defaults["baseline_update_strategy"] = clean_text(
        baseline.get("update_strategy") or defaults.get("baseline_update_strategy") or "new_label"
    ).lower()
    return defaults


def build_profile_map(doc: Dict[str, Any], suite_dir: str) -> Dict[str, Dict[str, Any]]:
    profiles = doc.get("profiles") or {}
    if not isinstance(profiles, dict) or not profiles:
        raise ValueError("Suite YAML must contain a non-empty 'profiles' mapping.")

    normalized: Dict[str, Dict[str, Any]] = {}
    for profile_name, raw_value in profiles.items():
        profile = raw_value or {}
        if not isinstance(profile, dict):
            raise ValueError(f"Profile '{profile_name}' must be a mapping.")
        config_file = resolve_path(suite_dir, profile.get("config_file"))
        if not config_file:
            raise ValueError(f"Profile '{profile_name}' is missing config_file.")
        normalized_profile: Dict[str, Any] = {
            "profile": profile_name,
            "config_file": config_file,
        }
        users_file = resolve_path(suite_dir, profile.get("users_file"))
        if users_file:
            normalized_profile["users_file"] = users_file
        normalized[profile_name] = normalized_profile
    return normalized


def materialize_scenario(
    suite_id: str,
    suite_dir: str,
    defaults: Dict[str, Any],
    profiles: Dict[str, Dict[str, Any]],
    raw: Dict[str, Any],
    explicit_id: str = "",
) -> Dict[str, Any]:
    profile_name = clean_text(raw.get("profile") or defaults.get("profile"))
    if not profile_name:
        raise ValueError("Each scenario requires a profile.")
    if profile_name not in profiles:
        raise ValueError(f"Scenario references unknown profile '{profile_name}'.")

    baseline_override = raw.get("baseline") or {}
    baseline_fields: Dict[str, Any] = {}
    if "enabled" in baseline_override:
        baseline_fields["baseline_enabled"] = baseline_override.get("enabled")
    if "mode" in baseline_override:
        baseline_fields["baseline_mode"] = baseline_override.get("mode")
    if "update_strategy" in baseline_override:
        baseline_fields["baseline_update_strategy"] = baseline_override.get("update_strategy")
    if "manifest" in baseline_override:
        baseline_fields["baseline_manifest"] = baseline_override.get("manifest")
    if "store_dir" in baseline_override:
        baseline_fields["baseline_store_dir"] = baseline_override.get("store_dir")
    if "policy" in baseline_override:
        baseline_fields["baseline_policy"] = baseline_override.get("policy")

    merged = merge_dicts(defaults, profiles[profile_name], raw, baseline_fields)
    merged["suite_id"] = suite_id
    merged["profile"] = profile_name
    merged["config_file"] = resolve_path(suite_dir, merged.get("config_file"))
    merged["users_file"] = resolve_path(suite_dir, merged.get("users_file"))
    merged["longrun_policy"] = resolve_path(
        suite_dir,
        merged.get("longrun_policy") or DEFAULT_LONGRUN_POLICY,
    )
    merged["op"] = normalize_op(merged.get("op"))
    merged["object_size"] = clean_text(merged.get("object_size") or merged.get("object_size_spec"))
    merged["baseline_mode"] = clean_text(merged.get("baseline_mode") or "off").lower()
    merged["baseline_enabled"] = parse_bool(merged.get("baseline_enabled"), merged["baseline_mode"] in ("generate", "compare"))
    merged["baseline_policy"] = resolve_path(suite_dir, merged.get("baseline_policy") or DEFAULT_PERF_POLICY)
    merged["baseline_manifest"] = resolve_path(suite_dir, merged.get("baseline_manifest") or DEFAULT_BASELINES_MANIFEST)
    merged["baseline_store_dir"] = resolve_path(suite_dir, merged.get("baseline_store_dir") or DEFAULT_BASELINE_STORE)
    merged["baseline_update_strategy"] = clean_text(
        merged.get("baseline_update_strategy") or "new_label"
    ).lower()

    if not merged["config_file"]:
        raise ValueError(f"Scenario under profile '{profile_name}' is missing config_file.")
    if not merged["op"]:
        raise ValueError(f"Scenario under profile '{profile_name}' is missing op.")
    if not merged["object_size"]:
        raise ValueError(f"Scenario under profile '{profile_name}' is missing object_size.")
    if merged.get("threads") in (None, ""):
        raise ValueError(f"Scenario under profile '{profile_name}' is missing threads.")

    try:
        merged["threads"] = int(merged["threads"])
    except ValueError as exc:
        raise ValueError(f"Scenario threads must be integer: {merged.get('threads')}") from exc

    run_seconds = merged.get("run_seconds")
    requests_per_thread = merged.get("requests_per_thread")
    merged["run_seconds"] = "" if run_seconds in (None, "") else int(run_seconds)
    merged["requests_per_thread"] = "" if requests_per_thread in (None, "") else int(requests_per_thread)
    merged["allow_open_ended_run"] = parse_bool(merged.get("allow_open_ended_run"), False)
    merged["enabled"] = parse_bool(merged.get("enabled"), True)
    merged["continue_on_fail"] = parse_bool(merged.get("continue_on_fail"), True)
    merged["analyze_longrun"] = parse_bool(merged.get("analyze_longrun"), False)
    merged["gate_longrun"] = parse_bool(merged.get("gate_longrun"), False)

    scenario_id = clean_text(explicit_id or raw.get("scenario_id"))
    if not scenario_id:
        scenario_id = (
            f"{slugify(profile_name)}_{slugify(merged['op'])}_"
            f"{int(merged['threads'])}t_{slugify(merged['object_size'])}"
        )
    merged["scenario_id"] = scenario_id
    return merged


def expand_matrix(
    suite_id: str,
    suite_dir: str,
    defaults: Dict[str, Any],
    profiles: Dict[str, Dict[str, Any]],
    matrix: Dict[str, Any],
) -> List[Dict[str, Any]]:
    if not matrix:
        return []

    profile_values = listify(matrix.get("profile")) or list(profiles.keys())
    op_values = listify(matrix.get("op")) or [defaults.get("op")]
    thread_values = listify(matrix.get("threads")) or [defaults.get("threads")]
    object_values = listify(matrix.get("object_size")) or [defaults.get("object_size")]

    scenarios: List[Dict[str, Any]] = []
    for profile_name, op, threads, object_size in itertools.product(
        profile_values,
        op_values,
        thread_values,
        object_values,
    ):
        scenario = materialize_scenario(
            suite_id,
            suite_dir,
            defaults,
            profiles,
            {
                "profile": profile_name,
                "op": op,
                "threads": threads,
                "object_size": object_size,
            },
        )
        scenarios.append(scenario)
    return scenarios


def load_suite(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        document = yaml.safe_load(handle) or {}
    if not isinstance(document, dict):
        raise ValueError("Suite YAML root must be a mapping.")
    return document


def write_resolved_yaml(path: str, doc: Dict[str, Any], scenarios: List[Dict[str, Any]]) -> None:
    payload = copy.deepcopy(doc)
    payload["resolved_scenarios"] = scenarios
    with open(path, "w", encoding="utf-8") as handle:
        yaml.safe_dump(payload, handle, allow_unicode=True, sort_keys=False)


def write_resolved_tsv(path: str, scenarios: List[Dict[str, Any]]) -> None:
    header = [
        "suite_id",
        "scenario_id",
        "profile",
        "config_file",
        "users_file",
        "op",
        "threads",
        "object_size_spec",
        "run_seconds",
        "requests_per_thread",
        "allow_open_ended_run",
        "continue_on_fail",
        "analyze_longrun",
        "gate_longrun",
        "longrun_policy",
        "baseline_mode",
        "baseline_enabled",
        "baseline_policy",
        "baseline_manifest",
        "baseline_store_dir",
        "baseline_update_strategy",
    ]
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\t".join(header) + "\n")
        for scenario in scenarios:
            row = [
                clean_text(scenario.get("suite_id")),
                clean_text(scenario.get("scenario_id")),
                clean_text(scenario.get("profile")),
                clean_text(scenario.get("config_file")),
                clean_text(scenario.get("users_file")),
                clean_text(scenario.get("op")),
                clean_text(scenario.get("threads")),
                clean_text(scenario.get("object_size")),
                clean_text(scenario.get("run_seconds")),
                clean_text(scenario.get("requests_per_thread")),
                "1" if parse_bool(scenario.get("allow_open_ended_run"), False) else "0",
                "1" if parse_bool(scenario.get("continue_on_fail"), True) else "0",
                "1" if parse_bool(scenario.get("analyze_longrun"), False) else "0",
                "1" if parse_bool(scenario.get("gate_longrun"), False) else "0",
                clean_text(scenario.get("longrun_policy")),
                clean_text(scenario.get("baseline_mode")),
                "1" if parse_bool(scenario.get("baseline_enabled"), False) else "0",
                clean_text(scenario.get("baseline_policy")),
                clean_text(scenario.get("baseline_manifest")),
                clean_text(scenario.get("baseline_store_dir")),
                clean_text(scenario.get("baseline_update_strategy")),
            ]
            handle.write("\t".join(row) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Resolve obs_c_bench suite YAML into a flat scenario manifest.")
    parser.add_argument("--suite", required=True, help="Path to suite YAML file")
    parser.add_argument("--print-suite-id", action="store_true", help="Print resolved suite id and exit")
    parser.add_argument("--resolved-tsv", help="Output path for resolved scenario TSV")
    parser.add_argument("--resolved-yaml", help="Output path for normalized/resolved suite YAML")
    parser.add_argument("--default-users", help="Default users.dat path when the suite does not provide one")
    args = parser.parse_args()

    suite_path = os.path.abspath(args.suite)
    suite_dir = os.path.dirname(suite_path)
    doc = load_suite(suite_path)
    suite_id = clean_text(doc.get("suite_id")) or slugify(os.path.splitext(os.path.basename(suite_path))[0])

    if args.print_suite_id:
        print(suite_id)
        return 0

    defaults = build_default_settings(doc, suite_dir)
    if args.default_users and not defaults.get("users_file"):
        defaults["users_file"] = os.path.abspath(args.default_users)
    profiles = build_profile_map(doc, suite_dir)

    scenarios = expand_matrix(
        suite_id,
        suite_dir,
        defaults,
        profiles,
        doc.get("matrix") or {},
    )

    for raw in doc.get("scenarios") or []:
        if not isinstance(raw, dict):
            raise ValueError("Each item under 'scenarios' must be a mapping.")
        scenarios.append(materialize_scenario(suite_id, suite_dir, defaults, profiles, raw))

    resolved: List[Dict[str, Any]] = []
    seen_ids = set()
    for scenario in scenarios:
        if not scenario.get("enabled", True):
            continue
        scenario_id = scenario["scenario_id"]
        if scenario_id in seen_ids:
            raise ValueError(f"Duplicate scenario_id detected in suite: {scenario_id}")
        seen_ids.add(scenario_id)
        resolved.append(scenario)

    if not resolved:
        raise ValueError("Suite did not produce any enabled scenarios.")

    if args.resolved_tsv:
        ensure_directory(os.path.dirname(os.path.abspath(args.resolved_tsv)))
        write_resolved_tsv(args.resolved_tsv, resolved)
    if args.resolved_yaml:
        ensure_directory(os.path.dirname(os.path.abspath(args.resolved_yaml)))
        write_resolved_yaml(args.resolved_yaml, doc, resolved)

    print(f"[+] Suite: {suite_id}")
    print(f"[+] Scenarios: {len(resolved)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"[-] Failed to resolve suite: {exc}", file=sys.stderr)
        sys.exit(2)
