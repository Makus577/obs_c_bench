# Agents for obs_c_bench

本文件用于帮助熟悉 `obs_c_bench` 仓库的开发者或代码代理快速进入上下文，减少重复探索，固化常见开发流程，并提高功能迭代与提交流程的一致性。

## Available Agents

### Default agent
- Purpose: 面向整个仓库的通用实现代理。
- Use cases: 修改 C 压测核心逻辑、调整构建系统、更新 Python 辅助脚本、补文档、拆分提交。

### Explore agent
- Purpose: 只读探索代理。
- Use cases: 快速定位功能入口、确认构建/测试命令、梳理代码结构、做变更前调研。

## When to use which agent
- 先用 `Explore`：
  定位 `main.c`、`worker.c`、`obs_adapter.c`、`config_loader.c`、`Makefile`、`scripts/` 下脚本与 `README.md` 的关系。
- 再用 `Default`：
  实现功能、修复构建、调整脚本路径、补测试、整理提交。

## Repo Map

### Core C code
- `src/main.c`
  入口、CLI 解析、配置加载、监控线程、最终汇总、报告归档。
- `src/worker.c`
  worker 执行主循环、请求时延统计、detail 日志落盘。
- `src/obs_adapter.c`
  OBS SDK 适配层、PUT/GET/DELETE/MULTIPART/RESUMABLE 具体调用与字节统计。
- `src/config_loader.c`
  `config.dat` / `users.dat` 解析，测试动作与对象大小等配置处理。
- `src/bench.h`
  关键结构体与公共声明，包含 `Config`、`ThreadStats`、`WorkerArgs`、`BenchmarkSummary`。

### Build and SDK
- `Makefile`
  支持 `make mock`、`make mock_asan`、`make`、`make asan`、`make sdk-bootstrap`。
- 真实 SDK 不再随仓库提交。
  默认通过 `OBS_SDK_ROOT` 或 `.deps/obs_sdk/<platform>/` 引入。

### Python helpers
- `scripts/tests/`
  编译与冒烟测试、功能测试。
- `scripts/sdk/`
  SDK bootstrap、临时凭证生成。
- `scripts/reporting/`
  明细日志合并、可视化看板生成。
- `scripts/data/`
  测试数据生成。

### Documentation and workflow
- `README.md`
  面向使用者的主文档，涉及 CLI、输出目录、采样指标、SDK 引入方式。
- `.agents/workflows/`
  面向代理/开发流程的辅助工作流文档。

## Current Product Facts

以下是当前仓库已经固化的关键能力，开发时应默认保持这些行为不回退：

- CLI 支持：
  `--config`、`--users`、`--op`、`--threads`、`--object-size`、`--output-dir`、`--log-dir`
- 输出目录默认分离：
  报告在 `reports/task_<timestamp>/`
  日志在 `logs/task_<timestamp>/`
- 报告文件：
  `archive.csv`、`brief.txt`
- 日志文件：
  `realtime.txt`、`detail_*.csv`
- 采样周期固定 3 秒，但短任务结束时必须补写最终样本。
- 性能归档指标包含：
  CPU、RSS、TPS、BPS、平均 latency、P99 latency、单流带宽。
- 字节统计分两套口径：
  `streamed_bytes` 用于实时采样
  `completed_success_bytes` 用于最终汇总
- 平均 latency 与 P99 只按成功请求统计。
- 正式性能采样目标平台是 Linux，支持 x86_64 / aarch64。
- 非 Linux 开发机允许 mock 联调，但 RSS 可能为 `-1`。

## Recommended Dev Flow

### 1. 改动前先确认上下文
- 看 `README.md`，确认用户可见行为。
- 看 `src/main.c`，确认入口、CLI、输出目录和 monitor 流程。
- 看 `src/worker.c` 和 `src/obs_adapter.c`，确认请求级统计口径。
- 如果改到脚本路径或构建链路，同时检查 `Makefile`、`scripts/` 和 `.agents/workflows/`。

### 2. 优先走 mock 路径联调
- 无真实 SDK 时先跑：
  `make mock`
- 需要整体回归时跑：
  `python3 scripts/tests/compile_and_smoke_test.py`
- 真实 SDK 缺失时：
  `make` 失败并给出清晰错误是预期行为，不应把它误判为 bug。

### 3. 真实 SDK 构建流程
- 推荐先执行：
  `make sdk-bootstrap`
- 或显式指定：
  `OBS_SDK_ROOT=/abs/path/to/sdk make`
- 不要再假设仓库根目录自带 `./lib` 或 `include/eSDKOBS.h`。

### 4. 改文档时同步校验
- 只要改了下列内容之一，就必须同步检查 `README.md`：
  CLI 参数
  输出目录
  指标口径
  SDK 引入方式
  Python 脚本路径
- 如果改了代理或测试脚本入口，同时同步检查 `.agents/workflows/`。

## Submission Workflow

推荐按“最小功能提交”拆分，避免一个提交混入不相干内容。

### 推荐的最小提交顺序
1. 核心功能提交
   包含 C 代码主逻辑，如 CLI、采样、归档、统计口径修正。
2. 构建治理提交
   包含 `Makefile`、`.gitignore`、SDK 引入方式调整。
3. 脚本与文档提交
   包含 `scripts/` 迁移、README、workflow 文档更新。

### 提交前验证顺序
- 核心功能后：
  `make mock`
- 构建治理后：
  `make mock`
  `make`
  说明：无 SDK 环境下，`make` 输出缺失 SDK 的明确提示即可。
- 脚本与文档后：
  `PYTHONPYCACHEPREFIX=/tmp/pycache_codex python3 -m py_compile scripts/tests/compile_and_smoke_test.py scripts/tests/functional_test.py scripts/sdk/update_sdk.py scripts/sdk/generate_temp_ak_sk.py scripts/data/gen_data.py scripts/reporting/merge_details.py scripts/reporting/plot_report.py`
  `make mock`
  `python3 scripts/tests/compile_and_smoke_test.py`

### 提交时的内容边界
- 如果只改功能，不要顺手带入 `.github/`、`AGENTS.md` 这类辅助文件，除非目标就是更新协作规范。
- 如果只是迁移脚本位置，必须同时更新：
  `src/main.c`
  `Makefile`
  `README.md`
  `.agents/workflows/`
- 脚本迁移的正确做法是：
  删除旧路径
  新增新路径
  更新全部引用
  不保留两份同功能脚本

## Common Pitfalls

### 1. README 和实现不一致
- 这是最常见问题之一。
- 特别容易遗漏：
  默认目录仍写成 `logs/task_*`
  旧脚本路径仍写在 README 或 workflow 中
  旧 SDK 假设仍写成 `./lib`

### 2. 只改脚本文件，不改调用入口
- 迁移 `generate_temp_ak_sk.py` 后，必须同步更新 `src/main.c`。
- 迁移 `update_sdk.py` 后，必须同步更新 `Makefile`。
- 迁移测试/报表脚本后，必须同步更新 README 和 `.agents/workflows/`。

### 3. 把实时带宽和最终带宽混为一谈
- `realtime.txt` 应该基于 `streamed_bytes`
- `archive.csv` / `brief.txt` 应该基于 `completed_success_bytes`
- 不要把失败上传或未完成请求的字节直接算进最终成功带宽。

### 4. 短任务无采样
- monitor 线程周期是 3 秒。
- 若任务在 3 秒内完成，结束时必须补采样一次。
- 修改 monitor 流程时不要回退这个行为。

### 5. 在非 Linux 环境误判 RSS/CPU 为异常
- 非 Linux mock 联调时，RSS 为 `-1` 是可接受行为。
- 正式采样口径只对 Linux 环境做保证。

### 6. 真实 SDK 构建失败被误判为仓库问题
- 若本地没有 OBS SDK，`make` 失败是预期。
- 优先使用：
  `make sdk-bootstrap`
  或 `OBS_SDK_ROOT=/path make`

## Efficiency Tips

- 查找入口优先：
  `rg "output-dir|log-dir|archive.csv|realtime.txt|streamed_bytes|completed_success_bytes|CLOCK_PROCESS_CPUTIME_ID|VmRSS" src README.md`
- 改统计口径时优先同时看：
  `src/worker.c`
  `src/obs_adapter.c`
  `src/main.c`
- 改脚本组织时优先同时看：
  `scripts/`
  `Makefile`
  `README.md`
  `.agents/workflows/`
- 回归时优先跑：
  `make mock`
  `python3 scripts/tests/compile_and_smoke_test.py`

## How to extend this file

如果后续继续沉淀新的稳定流程，优先补充以下内容：
- 新的默认行为或约束
- 新的最小提交拆分模板
- 新增脚本目录或构建入口
- 常见回归问题与排查命令

保持原则：
- 写仓库事实，不写临时讨论
- 写稳定流程，不写一次性操作
- 让第一次接手这个仓库的人也能直接按文档执行
