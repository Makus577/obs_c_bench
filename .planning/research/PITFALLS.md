# Pitfalls Research

**Domain:** OBS C Benchmark Tool Enhancement (Simplified UX, Workflow Integration, Extended Metrics)
**Researched:** 2026-04-19
**Confidence:** MEDIUM

## Critical Pitfalls

### Pitfall 1: Template-Generated Config Hides Behavioral Differences

**What goes wrong:**
Simplified templates produce configs that behave differently from manual configs in non-obvious ways. Users get "working" tests that measure different things than intended.

**Why it happens:**
Template generators abstract away 7 core concepts (suite_id/run_id/profile/scenario/scenario_id/baseline/longrun) but the underlying SDK behavior still depends on all of them. When a template fills in a "smart default," users don't realize which behavior they opted out of.

**How to avoid:**
- Template output MUST include a `--explain` flag that shows what fields were auto-filled and why
- Document every smart default with the behavioral implication
- Provide a `--verbose` run mode that shows resolved config including defaults

**Warning signs:**
- Users say "it worked before" after switching from manual to template config
- Baseline comparison fails with no obvious cause
- Mock mode results differ from Real SDK mode

**Phase to address:**
Phase 1 (Template Generator) must include config diff and explanation output.

---

### Pitfall 2: One-Click Workflow Masks Partial Failures

**What goes wrong:**
A "one-click" script that runs压测 → merge_details → analyze_longrun → perf_gate → plot_report silently skips steps when intermediate steps fail. User sees "success" but analysis is incomplete or based on stale data.

**Why it happens:**
Sequential scripts use exit codes but don't validate that output files are actually usable. If `perf_gate compare` fails, `plot_report` may run on old baseline data without warning.

**How to avoid:**
- Each step must validate its input files exist and are non-empty
- Use explicit file locks or markers to prevent stale data usage
- `--dry-run` mode that shows what would happen without executing
- Pass a "pipeline run ID" through all steps to detect cross-run contamination

**Warning signs:**
- `brief.txt` timestamps don't match `archive.csv` timestamps
- `realtime.txt` shows gaps in sampling (missing 3-second intervals)
- Plot output shows data from different run IDs overlaid

**Phase to address:**
Phase 2 (One-Click Workflow) must include input validation and stale-data detection.

---

### Pitfall 3: Metric Bloat Obscures Regression Signal

**What goes wrong:**
Extended metrics (new P99/P99.9 visualizations, RSS growth charts, TPS decay curves) look impressive but make it harder to answer "did performance get worse?"

**Why it happens:**
More metrics = more thresholds to set = more ways for a "normal" run to fail a gate. Teams respond by loosening thresholds or ignoring new metrics entirely, negating their value.

**How to avoid:**
- New metrics MUST be additive, not gate-changing, for at least one release
- Establish baseline values before adding thresholds
- Provide a "focus mode" that hides new metrics until explicitly requested

**Warning signs:**
- `perf_gate` starts failing more often without code changes
- Engineers disable new metrics within weeks of adding them
- Alert fatigue increases

**Phase to address:**
Phase 3 (Extended Metrics) must include metric lifecycle policy: additive-only for one release.

---

### Pitfall 4: Backward Compatibility Gaps in Config Loading

**What goes wrong:**
New template generator or simplified config format produces YAML that existing config loaders don't fully support. Existing scripts break silently.

**Why it happens:**
The C config_loader.c has intricate validation logic for `gm_auth_mode`, certificate paths, and range options. New YAML fields may be silently ignored instead of producing errors.

**How to avoid:**
- Add schema validation before config loading (Python pre-check)
- Instrument config_loader to warn on unknown fields
- Test simplified configs against every existing config_loader code path

**Warning signs:**
- New config works with mock mode but fails with Real SDK
- Certain certificate configurations silently use wrong defaults
- Range options produce unexpected behavior

**Phase to address:**
Phase 1 (Template Generator) must include config validation against existing loader.

---

### Pitfall 5: Silent Memory Growth in Long-Run Analysis

**What goes wrong:**
`analyze_longrun.py` or report scripts load all CSV data into memory. With large test runs (millions of rows), scripts hang or crash instead of producing reports.

**Why it happens:**
pandas memory usage scales with row count. A 1M row `archive.csv` can consume 2-4GB RAM in pandas. The 5% overhead budget for report generation can be exceeded by 10x.

**How to avoid:**
- Stream process CSV files with chunked reading
- Set explicit memory budgets and fail fast if exceeded
- Provide `--sample` flag for runs exceeding row limits

**Warning signs:**
- `plot_report.py` takes >30 seconds on "normal" sized runs
- Machine becomes unresponsive during report generation
- analyze_longrun.py OOMs on multi-GB archive.csv

**Phase to address:**
Phase 3 (Extended Metrics) must include memory-bounded processing design.

---

### Pitfall 6: Dashboard Proliferation Without Version Control

**What goes wrong:**
`plot_report.py` generates dashboards that get copied, modified, and diverged. Teams run different versions of "the same" dashboard and get different conclusions.

**Why it happens:**
Dashboard JSONs are treated as output, not source. No one tracks which dashboard version produced which conclusion.

**How to avoid:**
- Embed run metadata (run_id, timestamp, git commit) into dashboard filename
- Store dashboard templates in version control, not generated output
- Provide `--template-only` flag that outputs dashboard structure without data

**Warning signs:**
- "Which dashboard is correct?" debates in post-mortems
- Different teams show different numbers for the same run
- Dashboard changes aren't reviewed

**Phase to address:**
Phase 2 (One-Click Workflow) must include dashboard versioning and provenance.

---

### Pitfall 7: CI Gate Bypass When Metrics Look "Close Enough"

**What goes wrong:**
perf_gate thresholds are set loosely to avoid "false failures," but this allows genuine regressions to slip through. Engineers learn to ignore the gate.

**Why it happens:**
TPS variance between runs is high (10-20% is normal). Setting tight thresholds causes CI to fail on normal variance. Setting loose thresholds defeats the purpose.

**How to avoid:**
- Use statistical significance testing (e.g., Mann-Whitney U) not just threshold comparison
- Track threshold breach rate over time; alert if >5% of runs fail
- Require percentiles not just averages

**Warning signs:**
- perf_gate rarely fails even when performance concerns are raised
- Engineers dismiss perf_gate failures as "just variance"
- Regression bugs reach production

**Phase to address:**
Phase 3 (Extended Metrics) must include statistical threshold methodology.

---

### Pitfall 8: Simplified Config Still Requires Expert Knowledge

**What goes wrong:**
"Simplified" config still requires understanding of scenario vs. profile vs. suite, object name patterns, credential loading, and OBS semantics. Users hit walls immediately after template generation.

**Why it happens:**
The 7 core concepts exist because the tool models real SDK behavior. Abstraction without education just delays the learning curve.

**How to avoid:**
- Template generator MUST include inline context-sensitive help
- Provide `--interactive` mode that asks questions and explains each answer
- Build a "happy path" for the most common case (single scenario, single user)

**Warning signs:**
- Users immediately post questions about what fields mean
- Template output requires manual editing to work
- Documentation requires reading before any config makes sense

**Phase to address:**
Phase 1 (Template Generator) must include education-first design.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Hardcoding TPS threshold of 1000 | Simple perf_gate config | Breaks on faster machines; too tight on slower | Never for production gates |
| Using YAML anchors for shared profiles | DRY config | Anchor changes cascade invisibly | Only with explicit comment explaining cascade |
| `python3` without virtualenv | Works on developer machine | Dependency conflicts on CI | Never — use venv or container |
| Copying plot_report.py output | Quick iteration | Dashboard divergence | Only for one-off investigation |
| Skipping mock mode in CI | Faster feedback | SDK changes break without detection | Never |

---

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| OBS SDK version mismatch | Bootstrap pulls latest master, behavior changes | Pin to specific commit tag |
| Temporary AK/SK tokens | Tokens expire mid-long-run (1hr default) | Detect token age and warn; support refresh |
| config.dat + YAML mixed usage | Conflicting settings not detected | Validate one config source, reject mixing |
| Report scripts + non-ASCII paths | Matplotlib fails on Chinese/emoji paths | Sanitize paths or use UUIDs |

---

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| CSV streaming with pandas | Memory spikes during chunk boundaries | Use iterator API, not chunk API | archive.csv >500MB |
| Subprocess overhead in one-click | Each script forks new Python | Inline processing or use library calls | >10 scenarios per suite |
| Detail logging with many threads | FD exhaustion at ~1000 threads | Cap detail log threads; warn at high concurrency | High-load scenarios |
| matplotlib in headless mode | Poppler font errors on Linux | Install fonts explicitly or use Agg backend | CI containers |

---

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| AK/SK in template output | Credentials in version control | Use env vars or prompt; never write to output |
| Plot filenames with run IDs | Run ID enumeration | Use UUIDs, not sequential IDs |
| Certificate paths in config | Path traversal if config is user-provided | Validate paths exist before passing to SDK |
| Log files with full URLs | OBS URLs expose bucket names | Redact in log output |

---

## UX Pitfalls

| Pitfall | User Impact | Better Approach |
|---------|-------------|-----------------|
| Error: "Failed to load config" with no details | User doesn't know what to fix | Show field name, value, and expected format |
| Warning: "Baseline not found" treated as error | Blocks valid first runs | Distinguish missing-baseline from invalid-baseline |
| Silent fallback to defaults | User doesn't know config was wrong | Log what was wrong and what was assumed |
| Plot generation failure leaves partial output | User thinks report is complete | Use atomic write (temp file + rename) |

---

## "Looks Done But Isn't" Checklist

- [ ] **Template Generator:** Often missing validation against existing config_loader — verify by running output through existing tools
- [ ] **One-Click Workflow:** Often missing stale-data detection — verify by touching archive.csv timestamp
- [ ] **Extended Metrics:** Often missing baseline establishment — verify by checking metric history before adding gates
- [ ] **perf_gate:** Often missing statistical testing — verify by running known regression against it
- [ ] **plot_report.py:** Often missing font handling — verify by running in clean container
- [ ] **analyze_longrun.py:** Often missing memory bounds — verify by processing 10M row file

---

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Template config incompatibility | LOW | Re-run template with `--verbose`, compare resolved config |
| Stale data in pipeline | MEDIUM | Delete all output, re-run with fresh run_id |
| Metric bloat false failures | LOW | Disable new metrics temporarily, track issue |
| Memory OOM in report | HIGH | Kill process, run with `--sample` flag |
| Dashboard divergence | MEDIUM | Delete local copies, re-pull from version control |

---

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Template hides behavioral diffs | Phase 1: Template Generator | Run template vs manual config, compare SDK calls |
| One-click masks partial failures | Phase 2: Workflow Integration | Inject failure at each step, verify detection |
| Metric bloat | Phase 3: Extended Metrics | Monitor threshold breach rate over 10 runs |
| Backward compat gaps | Phase 1: Template Generator | Test new YAML on all existing code paths |
| Memory growth | Phase 3: Extended Metrics | Process 1M+ row CSV, verify bounded memory |
| Dashboard proliferation | Phase 2: Workflow Integration | All dashboards have run_id in filename |
| CI gate bypass | Phase 3: Extended Metrics | Run synthetic regression, verify detection |
| Simplified config still complex | Phase 1: Template Generator | User testing with new users |

---

## Sources

- Google Benchmark User Guide (Preventing Optimization, Reducing Variance) — https://google.github.io/benchmark/
- Google SRE Book: Monitoring Distributed Systems — https://sre.google/sre-book/monitoring-distributed-systems/
- Google SRE Book: Practical Alerting — https://sre.google/sre-book/practical-alerting/
- Google SRE Book: Testing Reliability — https://sre.google/sre-book/testing-reliability/
- Grafana Best Practices: Dashboard Design — https://grafana.com/docs/grafana/latest/best-practices/
- GitLab CI/CD YAML Configuration — https://docs.gitlab.com/ee/ci/yaml/
- 12factor.net: Configuration — https://12factor.net/
- obs_c_bench CONCERNS.md (existing codebase issues)

---
*Pitfalls research for: OBS C Benchmark Tool Enhancement*
*Researched: 2026-04-19*
