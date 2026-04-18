# Requirements

**Domain:** OBS C SDK Performance Benchmark Tool — Long-Run Stability Testing Enhancement
**Project:** obs_c_bench Enhancement
**Version:** 1.0

## Requirement Traceability

| REQ-ID | Requirement | Phase | Status |
|--------|-------------|-------|--------|
| REQ-01 | 简化 Suite 配置体验 — 提供场景模板生成器 | 1 - Configuration Simplification | Active |
| REQ-02 | 全流程一键串联 — 压测 → 采样 → 分析 → 对比 → 看板 | 2 - One-Click Workflow | Active |
| REQ-03 | 增强长稳分析 — analyze_longrun.py 支持更多指标 | 3 - Long-Run Enhancement + CI Gates | Active |
| REQ-04 | 简化场景配置维数 — 智能默认值 + 参数继承 | 1 - Configuration Simplification | Active |
| REQ-05 | CI 门禁增强 — 支持更多性能指标门禁 | 3 - Long-Run Enhancement + CI Gates | Active |

---

## v1 Requirements

### Phase 1: 配置简化 — 模板生成器 + 智能默认值

#### REQ-01: 场景模板生成器

**Type:** New Feature
**Phase:** 1
**Priority:** Must Have

**Description:**
提供 CLI 命令，一键生成标准场景配置文件。用户只需指定核心参数（操作类型、线程数、对象大小），自动生成完整 `scenario.yaml`。

**Acceptance Criteria:**
- [ ] `python -m obs_bench.cli template --op upload --threads 128 --object-size 1MB --output scenario_upload_128t.yaml` 生成可用配置
- [ ] 生成的配置包含所有必要字段（profile, users, op, threads, object_size, requests_per_thread, scenario_id）
- [ ] 支持 `--template` 指定模板类型（smoke/perf/longrun）
- [ ] 生成前自动校验参数合理性（threads > 0, object_size > 0）
- [ ] 向后兼容：生成的配置与现有 `scenario.yaml` 格式完全一致

**Notes:**
- 内部调用现有 `config_loader.c` 的参数校验逻辑
- 模板默认路径：`scripts/templates/` 目录

#### REQ-04: 智能默认值 + 参数继承

**Type:** Enhancement
**Phase:** 1
**Priority:** Must Have

**Description:**
简化 Suite YAML 的参数继承机制。用户只需为整个 suite 指定一次默认配置，各 scenario 只覆盖自己独特的字段。

**Acceptance Criteria:**
- [ ] `defaults:` 块支持所有高频字段（threads, object_size, run_seconds, requests_per_thread）
- [ ] scenario 块中可以省略已继承的字段
- [ ] 配置加载时正确合并 defaults 和 scenario 配置
- [ ] CLI 输出显式告知"哪些字段来自 defaults，哪些来自 scenario 覆盖"

**Notes:**
- 兼容现有 `scenario.yaml` 格式，不破坏向后兼容
- 继承优先级：scenario 显式值 > defaults > 硬编码默认值

---

### Phase 2: 全流程串联 — 一键工作流

#### REQ-02: 全流程一键串联

**Type:** New Feature
**Phase:** 2
**Priority:** Must Have

**Description:**
提供单一入口，自动串接：压测执行 → merge_details → analyze_longrun → perf_gate → plot_report。

**Acceptance Criteria:**
- [ ] `python -m obs_bench.cli run --config scenario.yaml --suite suite.yaml --output-dir ./reports` 一键执行完整流程
- [ ] 每步骤失败时停止并返回非零 exit code，报告已生成的部分
- [ ] 支持 `--skip-plot` 跳过绘图步骤（门禁失败时节省时间）
- [ ] 支持 `--dry-run` 仅验证配置，不执行压测
- [ ] 输出目录结构：`./reports/<run_id>/{benchmark,analysis,gate,dashboard}/`
- [ ] 最终输出 `summary.json` + `summary.md`，包含所有阶段的 key metrics

**Notes:**
- 内部调用 `subprocess.run()` 封装现有脚本
- 编排脚本位于 `scripts/orchestration/run_benchmark.py`
- 不修改现有 `scripts/reporting/*.py`

**Data Flow:**
```
scenario.yaml → obs_c_bench
                    ↓
              logs/<id>/detail_*.csv → merge_details.py → detail.csv
                    ↓
              archive.csv → perf_gate.py → gate_result.json
              realtime.txt + archive.csv → analyze_longrun.py → longrun_summary.json
                    ↓
              detail.csv → plot_report.py → dashboard.png
```

---

### Phase 3: 长稳增强 + CI 门禁

#### REQ-03: 增强长稳分析

**Type:** Enhancement
**Phase:** 3
**Priority:** Should Have

**Description:**
增强 `analyze_longrun.py`，支持更多指标和可视化。

**Acceptance Criteria:**
- [ ] 新增 **CPU 漂移分析**：前段均值 vs 后段均值，输出漂移百分比
- [ ] 新增 **TPS 衰减分析**：首个 10% 时间窗口 vs 末 10% 时间窗口的 TPS 对比
- [ ] 新增 **成功率最小值追踪**：记录 success_rate 最低的时间点
- [ ] 新增长稳可视化：`longrun_trend.png` 包含 RSS/TPS/成功率 三轴时序图
- [ ] 输出 `longrun_summary.json` + `longrun_summary.md` 双格式

**Notes:**
- 新指标必须 additive-only（不修改已有 JSON schema，只新增字段）
- 内存处理：使用 pandas chunked reading 处理大文件

#### REQ-05: CI 门禁增强

**Type:** Enhancement
**Phase:** 3
**Priority:** Should Have

**Description:**
增强 `perf_gate.py`，支持更多性能指标门禁。

**Acceptance Criteria:**
- [ ] 新增 **内存门禁**：`avg_rss_mb`、`peak_rss_mb` 支持阈值比较
- [ ] 新增 **CPU 门禁**：`avg_cpu_pct`、`peak_cpu_pct` 支持阈值比较
- [ ] 新增 **单流带宽门禁**：`avg_single_stream_bps`、`max_single_stream_bps` 支持阈值比较
- [ ] 新增 **统计显著性测试**：TPS/ latency 对比时使用 t-test 或 Mann-Whitney U，p < 0.05 才判定为回归
- [ ] 门禁策略文件（`gate_policy.json`）支持新的指标定义
- [ ] `perf_gate_result.json` 输出 `statistical_significance` 字段

**Notes:**
- 保持 `perf_gate.py` CLI 参数兼容，不破坏现有调用方式
- 统计测试仅在样本量足够时启用（n >= 30），否则回退到简单阈值比较

---

## v2 Requirements (Deferred)

以下需求暂不纳入 v1 scope：

| ID | Requirement | Reason for Deferral |
|----|-------------|---------------------|
| REQ-v2-01 | Cross-run 趋势追踪（n 次运行的纵向对比） | 需要 baseline 版本历史，超出当前相邻对比模型 |
| REQ-v2-02 | 交互式配置向导（wizard-style CLI） | Phase 1 模板 + defaults 已足够降低摩擦 |
| REQ-v2-03 | 实时流式看板（WebSocket dashboard） | 增加运维复杂度，当前 post-run 看板已够用 |

---

## Out of Scope

以下明确不做的功能：

| Exclusion | Reason |
|-----------|--------|
| 修改 C 核心压测引擎 | 现有 lock-free 实现已满足性能要求 |
| 多语言 SDK benchmarking | 本项目聚焦 C SDK |
| 国密/国际站认证实现 | 已有 GmAuthMode 配置 |
| 动态线程调整（auto-scaling threads） | 改变测量基准，不符合 benchmark 原则 |
| 测试用例版本管理（数据库） | 文件 Git 版本化已够用 |

---

## Requirement Quality Checklist

- [ ] All requirements are **specific and testable**
- [ ] All requirements are **user-centric** ("用户可以 X"，不是"系统做 Y"）
- [ ] All requirements are **atomic**（一个 requirement 一个能力）
- [ ] All requirements are **independent**（最小化依赖关系）
- [ ] All acceptance criteria are **observable**（可验证）
- [ ] Traceability matrix is **complete**（每个 REQ-ID 映射到 Phase）
