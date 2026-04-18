# obs_c_bench 增强：长稳测试与场景对比

## What This Is

`obs_c_bench` 是一款华为云 OBS C SDK 工业级压测工具。当前已具备 Suite 模式、长稳分析、Baseline 对比能力。本项目聚焦于三个核心增强：**简化场景配置体验**、**全流程一键串联**、**长稳分析增强**，让压测工具更易于使用和维护。

## Core Value

让 performance benchmarking 从「配置复杂」到「一键可复现」，建立可持续的性能追踪体系。

## Requirements

### Validated

- ✓ **Suite 多场景编排** — 同一进程内顺序执行多场景，支持 profile 继承
- ✓ **实时监控采样** — Monitor Thread 每 3 秒采样 CPU/RSS/TPS/带宽，含区间值和累计值
- ✓ **P99/P99.9 长尾统计** — 基于在线直方图近似计算
- ✓ **Mock SDK 模式** — 无网络情况下验证逻辑正确性
- ✓ **Longrun 分析脚本** — `analyze_longrun.py` 分析 RSS 增长、TPS 衰减、成功率漂移
- ✓ **Baseline 对比门禁** — `perf_gate.py` 支持 `generate`/`compare` 两种模式
- ✓ **可视化看板** — `plot_report.py` 生成 2x2 Dashboard（散点图、CDF、TPS/BW 趋势、状态码分布）

### Active

- [ ] **REQ-01**: 简化 Suite 配置体验 — 提供场景模板生成器，减少重复配置
- [ ] **REQ-02**: 全流程一键串联 — 压测 → 采样 → 分析 → 对比 → 看板一键完成
- [ ] **REQ-03**: 增强长稳分析 — `analyze_longrun.py` 支持更多指标和可视化
- [ ] **REQ-04**: 简化场景配置维数 — 智能默认值 + 参数继承，减少用户必须指定的字段
- [ ] **REQ-05**: CI 门禁增强 — 支持更多性能指标门禁

### Out of Scope

- 修改 C 核心压测引擎（lock-free 架构、atomic counters）— 现有实现已满足性能要求
- OBS SDK 本身代码修改 — 本项目聚焦工具层
- 国密/国际站认证实现 — 已有 `GmAuthMode` 配置，保持现有实现
- 多语言 SDK benchmarking — 仅针对 C SDK

## Context

### 现有能力
- **C 核心**: 多线程 worker + Monitor Thread，数据通过 atomic counters 无锁更新
- **配置**: 支持 `config.dat`（传统）、`scenario.yaml`/`simple_config.yaml`（简化）
- **SDK 适配**: 双模式（Real SDK / Mock SDK），通过 `MOCK_SDK_MODE` 切换
- **报告**: `archive.csv`（机器可读）、`brief.txt`（人工可读）、`realtime.txt`（采样时序）

### 已知的配置痛点
- `scenario.yaml` 模板复杂，7 个核心概念（suite_id/run_id/profile/scenario/scenario_id/baseline/longrun）需要理解
- Suite YAML 中 `matrix` 展开虽然强大，但新手上手成本高
- 各脚本独立运行（压测 → merge_details → analyze_longrun/perf_gate → plot_report），缺乏串联

### 技术约束
- **平台**: Linux x86_64 / aarch64（正式压测），macOS（仅 mock 联调）
- **Python 依赖**: pandas、matplotlib、numpy、PyYAML（报告生成）
- **SDK**: OBS C SDK（通过 `OBS_SDK_ROOT` 或 `make sdk-bootstrap` 接入）

## Constraints

- **向后兼容**: 现有 `config.dat`、`scenario.yaml` 格式必须继续支持，不能破坏已有工作流
- **一键体验**: 增强不能引入新的复杂依赖，所有脚本在 `python3` 标准库 + 已有 pip 依赖下可运行
- **性能开销**: 报告生成和分析脚本本身的执行时间应 < 压测时间的 5%

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Suite YAML 保持扩展性 | 高级用户依赖完整 matrix 展开能力 | ✓ Good — 不简化高级用法 |
| 模板生成仅用于简化初级体验 | 新手不需要理解全部 7 个概念 | — Pending — 待验证 |
| 全流程脚本用 Python 实现 | 与现有报告脚本统一技术栈 | ✓ Good |
| 长稳分析输出保持 JSON+MD 双格式 | 机器可解析 + 人工可读 | ✓ Good |

---

*Last updated: 2026-04-19 after project initialization*
