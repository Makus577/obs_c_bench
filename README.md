# OBS C SDK Benchmark Tool (obs_c_bench)

`obs_c_bench` 是一款专为华为云对象存储服务 (OBS) 打造的工业级、高性能 C 语言压测工具。它基于官方 OBS C SDK 开发，采用了极致的无锁并发架构，旨在帮助开发者、架构师和测试工程师准确评估云存储后端的极限 TPS、吞吐带宽以及长尾时延表现。

当前版本已支持：

* 通过 CLI 显式指定 `config.dat` / `users.dat`
* 用命令行覆盖操作类型、总并发数、对象大小
* 通过 CLI 分别指定报告导出目录和日志目录
* 通过 `archive.csv`、基线清单与门禁脚本做性能基线对比
* 通过 `--suite <suite.yaml>` 在同一进程内顺序执行多场景测试计划
* 在 Linux x86 / ARM 环境下采集 CPU、RSS、TPS、BPS、平均时延、P99 时延、单流带宽
* 默认将报告输出到 `reports/`，日志输出到 `logs/`
* 通过 `make sdk-bootstrap` 或 `OBS_SDK_ROOT=/path make` 显式引入真实 OBS C SDK

## 🌟 核心特性 (Key Features)

* **极致的并发性能 (Lock-Free Architecture)**
* Worker 线程执行请求及本地统计数据收集时**全程无锁**，榨干压测机每一滴 CPU 性能。
* 独立的旁路监控线程（Monitor Thread）每 3 秒无锁采集全局状态，实时输出 CPU、内存、TPS、带宽与成功率；控制台中的 `Window TPS/BW` 表示最近一个采样窗口的吞吐，`Avg TPS/BW` 表示自任务开始以来的累计平均吞吐，其中控制台 TPS 会四舍五入显示为整数 `req/s`，带宽会自动用 `Bytes/s`、`KB/s`、`MB/s`、`GB/s` 等单位展示；若任务在 3 秒内结束，会自动补写最终样本，避免短任务无采样数据。


* **确定性伪随机打散 (LCG Hash Naming)**
* 内置标准 LCG (Linear Congruential Generator) 算法。开启 `ObjNamePatternHash=true` 后，可在对象名前缀生成均匀离散的 Hash 值，**彻底消除云存储底层分片的热点瓶颈**。
* 算法具备强确定性，确保先执行 PUT 压测后，再次执行 GET 压测能够 100% 精确命中已上传的对象。


* **零拷贝异构数据校验 (Zero-Copy Validation)**
* 支持 `EnableDataValidation=true`。工具在内存中预分配 1MB 的确定性特征环形缓冲区（Pattern Buffer）。
* 下载过程在网络回调层实时计算绝对偏移量，进行异构比对。即使并发 Range 下载，也能在极低 CPU 消耗下完成严格的**数据一致性校验**，精准捕获静默错误 (DataConsistencyError)。


* **智能多租户桶路由 (Smart Bucket Routing)**
* 支持 `users.dat` 批量加载多账户。
* 动态桶名拼接策略：自动按照 `{ak_lowercase}.{BucketNamePrefix}` 的格式将流量路由至各账户的专属桶，或通过 `BucketNameFixed` 强制打向固定桶。


* **防爆内存的海量流水落盘 (Log Rotation)**
* 开启 `EnableDetailLog=true` 后，支持请求级明细流水落盘。
* 工具自动按“标签优先、时间戳下沉一层”的方式创建隔离目录；单场景默认类似 `reports/upload_1mb_2t/20260224_120000/`，suite 默认类似 `reports/<suite_id>/20260224_120000/`。
* 单线程流水文件达到 1,000,000 行自动滚动切分（Rotation），防范长时间高并发测试导致的磁盘爆满与后处理 OOM。


* **一键式可视化看板 (Automated Dashboard)**
* 提供配套的 Python 脚本，支持海量分片日志的一键合并、时延排序及 P99/P99.9 长尾计算。
* 自动生成包含散点图、CDF 累积分布、TPS/带宽趋势、状态码占比的 2x2 高清诊断看板。



---

## 🛠️ 编译与安装 (Build & Install)

### 依赖项

* Linux 环境 (CentOS / Ubuntu 等)
* 支持 x86_64 / aarch64 Linux
* GCC 编译器 (支持 C99/GNU99 标准)
* 华为云 OBS C SDK (`eSDKOBS`) 及对应的 `libcurl`, `openssl` 动态库
* Python 3.x 及 `pandas`, `matplotlib`, `PyYAML` (用于图表生成、suite 解析与长稳分析)

> 说明：运行期 CPU / RSS 采样依赖 Linux `/proc` 与 POSIX 时间接口，因此正式压测环境应为 Linux。开发机在非 Linux 环境下可以完成部分 mock 联调，但内存指标可能为空或显示 `-1`。
>
> 说明：作为开源仓库，本项目默认**不提交**真实 OBS C SDK 的头文件和二进制库。真实构建时请使用 `make sdk-bootstrap` 自动下载构建，或通过 `OBS_SDK_ROOT` 指向你已经安装好的 SDK。

### 编译命令

```bash
# 清理历史产物
make clean_objs

# 编译 Mock 版 (仅用于联调逻辑，不发起真实网络请求)
make mock

# 编译 ASAN 版本 (用于检测内存错误)
make mock_asan

# 一键下载并构建官方 OBS C SDK 到本地缓存
make sdk-bootstrap

# 编译标准版 (连接真实 OBS 环境)
make

# 编译 ASAN 版本 (连接真实 OBS 环境)
make asan

```

编译成功后，将在根目录生成可执行文件 `obs_c_bench`（或 `obs_c_bench_mock`）。

### 推荐构建路径

#### 1. 无真实 SDK 时，先用 mock 联调

```bash
make mock
./obs_c_bench_mock --config ./config.dat --op upload
```

#### 2. 需要真实 OBS 压测时，一键 bootstrap

```bash
make sdk-bootstrap
make
```

默认缓存目录为：

```text
.deps/obs_sdk/<platform>/
```

其中 `<platform>` 形如 `linux-x86_64`、`linux-aarch64`。

#### 3. 使用已有 SDK 构建

如果你的机器上已经安装好了 OBS C SDK，可直接指定：

```bash
OBS_SDK_ROOT=/abs/path/to/sdk make
OBS_SDK_ROOT=/abs/path/to/sdk make asan
```

SDK 根目录需要包含：

```text
<sdk-root>/
  include/eSDKOBS.h
  lib/libeSDKOBS.so
```

### SDK Bootstrap

如果需要下载、编译官方 OBS C SDK 并写入本地缓存，可以使用：

```bash
make sdk-bootstrap
```

底层会调用 `scripts/sdk/update_sdk.py`，自动执行以下操作：
1. **架构检测**：自动识别系统架构 (x86_64 / aarch64)。
2. **SDK 下载**：从官方 GitHub 源码仓库下载 SDK 源码。
3. **本地编译**：按当前架构执行上游构建脚本。
4. **缓存产物**：将 `include/` 和 `lib/` 提取到 `.deps/obs_sdk/<platform>/`。

如需自定义输出目录，也可直接调用：

```bash
python3 scripts/sdk/update_sdk.py --output-root /abs/path/to/sdk
```

### 为什么不直接把 SDK 提交进仓库

这是一个开源代码仓，最佳实践是不把官方 SDK 的真实头文件和二进制库直接提交到项目源码树中。这样做有几个好处：

* 降低仓库体积和二进制污染
* 避免把上游 SDK 的发布/分发责任耦合进本项目
* 保持 mock 开发和真实构建两条路径都清晰
* CI、本地开发和生产环境都可以通过统一的 `OBS_SDK_ROOT` 契约接入

如果团队内部确实需要固定上游源码版本，可以额外使用 submodule，但不建议把 submodule 作为本开源仓库的默认使用方式。

---

## 🚀 快速开始 (Quick Start)

### 1. 配置凭证 (`users.dat`)

在根目录创建或编辑 `users.dat`，配置需要参与压测的账户信息（格式：`用户名, AK, SK`）：

```text
user1, YOUR_AK_1, YOUR_SK_1
user2, YOUR_AK_2, YOUR_SK_2

```

### 2. 调整测试计划 (`config.dat`)

编辑 `config.dat`，设置目标 Endpoint、并发量及测试动作。核心参数如下：

* `Users=1`：加载 `users.dat` 中的几个用户。
* `ThreadsPerUser=1000`：每个用户启动的并发线程数。
* `TestCase=201`：压测动作 (201=PUT, 202=GET, 204=DELETE, 216=MULTIPART, 230=RESUMABLE, 900=MIX)。
* `ObjectSize=4096`：测试对象的大小 (支持范围配置，如 `1024~4096`)。
* `RequestsPerThread=10000` 或 `RunSeconds=300`：退出条件限制。
* `AllowOpenEndedRun=true`：仅当你明确需要手动 `Ctrl+C` 停止时才开启开放式运行；默认不允许 `RunSeconds<=0` 且 `RequestsPerThread<=0` 的组合。

*(注：详细配置说明请参考 `config.dat` 文件内的中文注释)*

### 3. 执行压测

工具默认读取当前目录下的 `config.dat`：

```bash
./obs_c_bench

```

默认输出目录会天然分离：

* 报告目录：`reports/<scenario_label>/<timestamp>/`
* 日志目录：`logs/<scenario_label>/<timestamp>/`

其中 `<scenario_label>` 优先使用 `--scenario-id`，否则程序会根据操作类型、对象大小和线程数自动生成类似 `upload_1mb_128t` 的标签。

也可通过 CLI 参数快速覆盖 TestCase，方便脚本串联执行 (例如先跑 201 PUT，再跑 202 GET)：

```bash
./obs_c_bench 201
./obs_c_bench 202

```

也支持显式指定配置文件、用户文件和覆盖关键参数：

```bash
./obs_c_bench \
  --config ./config.dat \
  --users ./users.dat \
  --op upload \
  --threads 128 \
  --object-size 1MB \
  --output-dir ./reports_out \
  --log-dir ./runtime_logs
```

其中 `--object-size` 支持纯字节数和十进制单位范围，例如 `512KB`、`1MB`、`1MB~16MB`。

执行前会做配置合理性校验。若 `RunSeconds<=0` 且 `RequestsPerThread<=0`，工具默认会直接失败，避免误跑成无终止条件；只有在 `config.dat` 中显式设置 `AllowOpenEndedRun=true`，或在 suite YAML 中显式设置 `allow_open_ended_run: true` 时，才允许开放式运行。

### 4. CLI 参数说明

| 参数 | 说明 | 示例 |
| --- | --- | --- |
| `--config <path>` | 指定配置文件路径，默认 `config.dat` | `--config ./conf/config.dat` |
| `--users <path>` | 指定用户文件路径，默认 `users.dat` | `--users ./accounts/users.dat` |
| `--op <value>` | 覆盖操作类型，支持 `upload/download/delete/multipart/resumable/mix` 或 `201/202/204/216/230/900` | `--op upload` |
| `--threads <N>` | 覆盖总并发线程数；多用户时会按用户均分，余数从前往后补齐 | `--threads 1024` |
| `--object-size <spec>` | 覆盖对象大小，支持纯字节数、十进制单位和区间 | `--object-size 1MB~16MB` |
| `--scenario-id <id>` | 显式指定固定压测场景 ID，便于性能基线对比和 CI 门禁 | `--scenario-id upload_1mb_128t` |
| `--suite <path>` | 进入 suite 模式，在同一主进程内顺序执行 YAML 定义的多场景测试计划 | `--suite ./ci/perf/suites/auth_matrix.yaml` |
| `--output-dir <path>` | 仅控制报告导出目录，作用于 `archive.csv`、`brief.txt` | `--output-dir /data/reports` |
| `--log-dir <path>` | 仅控制日志目录，作用于 `realtime.txt`、`detail_*.csv` | `--log-dir /data/logs` |

参数优先级固定为：`CLI > config.dat > 默认值`。

### 4.1 Suite 模式

当你需要在一次执行里连续跑多组小场景时，可以使用：

```bash
./obs_c_bench \
  --suite ./ci/perf/suites/auth_matrix.yaml \
  --output-dir ./suite_reports \
  --log-dir ./suite_logs
```

`--suite` 是 suite 模式唯一新增 CLI。认证模式、协议、证书和 `GmAuthMode` 等基础环境差异继续放在各自 `config.dat` 中；并发数、PUT/GET、对象大小等压测变量则放在 suite YAML 中描述。

先理解 7 个核心概念：

| 概念 | 是否在 YAML 中配置 | 含义 | 例子 |
| --- | --- | --- | --- |
| `suite_id` | 是 | 一组场景的测试计划名 | `auth_matrix` |
| `run_id` | 否 | 一次 suite 实际执行的运行编号，由程序启动时自动生成 | `20260318_220054` |
| `profile` | 是 | 一类基础环境模板，通常绑定一个 `config.dat` | `gm_mutual` |
| `scenario` | 是 | suite 中的一条具体测试场景 | `gm_mutual_get_1mb_longrun` |
| `scenario_id` | 是 | 单个 scenario 的稳定标识，用于输出目录和 baseline 主键 | `upload_1mb_32t` |
| `baseline` | 是 | 当前 suite 是否生成 baseline 或与 baseline 对比 | `mode: generate` |
| `longrun` | 是 | 当前 suite 是否做长稳分析与长稳门禁 | `gates.longrun.enabled: true` |

其中：

| 运行时概念 | 作用 | 用户是否手工配置 |
| --- | --- | --- |
| `suite_id` | 决定 suite 报告根目录名，如 `reports/<suite_id>/...` | 需要 |
| `run_id` | 区分同一个 suite 的不同执行批次，如 `reports/<suite_id>/<run_id>/...` | 不需要，程序自动生成 |
| `scenario_id` | 决定单场景目录名，也作为 baseline 主键 | 需要保持稳定 |

推荐的最小 suite YAML 结构如下：

```yaml
suite_id: auth_matrix
description: inter/gm auth matrix benchmark
defaults:
  users_file: ./users.dat
  requests_per_thread: 100
profiles:
  inter_oneway:
    config_file: ./configs/inter_oneway.dat
  inter_mutual:
    config_file: ./configs/inter_mutual.dat
  gm_oneway:
    config_file: ./configs/gm_oneway.dat
  gm_mutual:
    config_file: ./configs/gm_mutual.dat
matrix:
  profile: [inter_oneway, inter_mutual, gm_oneway, gm_mutual]
  op: [upload, download]
  threads: [64, 128]
  object_size: [1MB, 4MB]
scenarios:
  - scenario_id: gm_mutual_get_1mb_longrun
    profile: gm_mutual
    op: download
    threads: 128
    object_size: 1MB
    analyze_longrun: true
baseline:
  enabled: true
  mode: compare
  manifest: ./ci/perf/baselines.csv
  store_dir: ./ci/perf/baseline_archives
  policy: ./ci/perf/gate_policy.json
  update_strategy: new_label
gates:
  longrun:
    enabled: true
    fail_on_regression: true
    policy: ./ci/perf/longrun_policy.yaml
reporting:
  continue_on_fail: true
```

当前 suite 常用字段建议按下面这张表理解：

| 字段 | 是否必填 | 含义 | 默认值 | 典型场景 |
| --- | --- | --- | --- | --- |
| `suite_id` | 是 | 整个测试计划名称 | 无 | `auth_matrix` |
| `defaults.users_file` | 否 | 默认用户文件 | 若传 `--users`，则使用 CLI 覆盖；否则空 | 所有场景共用一个 `users.dat` |
| `defaults.requests_per_thread` | 否 | 每线程请求数 | 空 | 定量短压测 |
| `defaults.run_seconds` | 否 | 每场景运行秒数 | 空 | 限时压测 |
| `defaults.allow_open_ended_run` | 否 | 是否允许无终止条件运行 | `false` | 明确要手动停止的开放式压测 |
| `profiles.<name>.config_file` | 是 | 某类基础环境配置文件 | 无 | 国际/国密、单向/双向认证 |
| `profiles.<name>.users_file` | 否 | 某 profile 的专属用户文件 | 继承 `defaults.users_file` | 不同认证环境使用不同账号 |
| `matrix.profile` | 否 | 自动展开的 profile 维度 | 所有 profile | 批量场景 |
| `matrix.op` | 否 | 自动展开的操作类型 | 空 | `upload/download` 批量展开 |
| `matrix.threads` | 否 | 自动展开的并发数 | 空 | `16/64/128` |
| `matrix.object_size` | 否 | 自动展开的对象大小 | 空 | `1MB/4MB/16MB` |
| `scenarios[].scenario_id` | 建议填 | 单场景稳定 ID | 自动推导 | baseline、报告目录 |
| `scenarios[].profile` | 是 | 选择哪个 profile | 无 | 绑定某个 `config.dat` |
| `scenarios[].op` | 是 | 单场景操作 | 无 | `upload` 或 `download` |
| `scenarios[].threads` | 是 | 单场景总并发 | 无 | `32` |
| `scenarios[].object_size` | 是 | 单场景对象大小 | 无 | `1MB` |
| `scenarios[].run_seconds` | 否 | 覆盖默认运行秒数 | 继承 `defaults.run_seconds` | 长稳或限时压测 |
| `scenarios[].requests_per_thread` | 否 | 覆盖默认请求数 | 继承 `defaults.requests_per_thread` | 精细控制单场景请求量 |
| `scenarios[].allow_open_ended_run` | 否 | 覆盖是否允许开放式运行 | 继承 `defaults.allow_open_ended_run` | 单场景手动停止测试 |
| `scenarios[].analyze_longrun` | 否 | 是否生成长稳分析 | `false` | 长稳测试 |
| `scenarios[].gate_longrun` | 否 | 是否让长稳分析影响最终结论 | `false` | 长稳门禁 |
| `scenarios[].enabled` | 否 | 是否启用该场景 | `true` | 暂时关闭某个场景 |
| `baseline.enabled` | 否 | 是否启用 baseline 能力 | `false` | baseline 生成/对比 |
| `baseline.mode` | 否 | `off/generate/compare` | `off` | 生成或对比 baseline |
| `baseline.manifest` | 否 | baseline 清单文件 | `./ci/perf/baselines.csv` | 基线登记 |
| `baseline.store_dir` | 否 | baseline 归档目录 | `./ci/perf/baseline_archives` | 存放 baseline archive |
| `baseline.policy` | 否 | 性能门禁策略 | `./ci/perf/gate_policy.json` | perf gate |
| `baseline.update_strategy` | 否 | baseline 更新策略 | `new_label` | 保留历史基线或覆盖 |
| `gates.longrun.enabled` | 否 | 是否启用长稳分析 | `false` | 长稳测试 |
| `gates.longrun.fail_on_regression` | 否 | 长稳退化是否判失败 | `false` | 长稳门禁 |
| `gates.longrun.policy` | 否 | 长稳策略文件 | `./ci/perf/longrun_policy.yaml` | 长稳阈值 |
| `reporting.continue_on_fail` | 否 | 某场景失败后是否继续后续场景 | `true` | 长 suite 批跑 |

若同时传了 `--users`，它会作为 suite 的默认 `users_file` 覆盖，除非 profile 或 scenario 显式指定了自己的用户文件。

典型模板索引如下：

| 场景 | 示例文件 | 适用目的 |
| --- | --- | --- |
| 简单性能测试 / 配置校验 | [`ci/perf/suites/examples/smoke_check.yaml`](/Users/wuchengqi/huaweicloud/obs_c_bench/ci/perf/suites/examples/smoke_check.yaml) | 快速确认 `config.dat`、`users.dat`、报告输出是否正常 |
| 性能测试并生成 baseline | [`ci/perf/suites/examples/perf_generate_baseline.yaml`](/Users/wuchengqi/huaweicloud/obs_c_bench/ci/perf/suites/examples/perf_generate_baseline.yaml) | 首次生成稳定性能基线 |
| 性能测试并对比 baseline | [`ci/perf/suites/examples/perf_compare_baseline.yaml`](/Users/wuchengqi/huaweicloud/obs_c_bench/ci/perf/suites/examples/perf_compare_baseline.yaml) | SDK 或参数优化后的性能对比 |
| 长稳测试 | [`ci/perf/suites/examples/longrun_test.yaml`](/Users/wuchengqi/huaweicloud/obs_c_bench/ci/perf/suites/examples/longrun_test.yaml) | 观察内存泄漏、吞吐衰减、成功率漂移 |
| 完整矩阵测试 | [`ci/perf/suites/auth_matrix.yaml`](/Users/wuchengqi/huaweicloud/obs_c_bench/ci/perf/suites/auth_matrix.yaml) | 四种认证场景 × 操作 × 并发 × 对象大小的批量测试 |

### 5. 对象大小写法

`ObjectSize` 与 `--object-size` 采用相同语法：

* 固定大小：`4096`
* 十进制单位：`512KB`、`1MB`、`2GB`
* 范围写法：`1024~4096`、`1MB~16MB`

当前单位按十进制换算：

* `1KB = 1000 Bytes`
* `1MB = 1000 * 1000 Bytes`
* `1GB = 1000 * 1000 * 1000 Bytes`

### 6. 执行前配置校验

为了避免误把任务跑成长期不退出，工具会在启动 worker 之前做统一校验。默认会拦截以下高风险配置：

| 校验项 | 默认行为 |
| --- | --- |
| `RunSeconds<=0` 且 `RequestsPerThread<=0` | 失败，除非显式开启 `AllowOpenEndedRun=true` / `allow_open_ended_run: true` |
| `threads<=0` 或 `ThreadsPerUser<=0` | 失败 |
| `RequestsPerThread<0` 或 `RunSeconds<0` | 失败 |
| 非法 `TestCase` | 失败 |
| `ObjectSize<=0` 或对象大小范围非法 | 失败 |
| `PartSize<=0` | 失败 |
| `multipart` 但 `PartsForEachUploadID<=0` | 失败 |
| `resumable` 但 `UploadFilePath` 为空 | 失败 |
| `mix` 但 `MixOperation` 为空 | 失败 |

开放式运行只适合你明确知道需要手动停止的场景。启用后，控制台和 `brief.txt` 会显示：

* `ExecutionMode: Open-Ended`
* `OpenEndedRun: ENABLED (explicit override)`

---

## 📊 日志与数据可视化 (Logging & Visualization)

当您在 `config.dat` 中配置了 `EnableDetailLog=true` 后，工具将为您提供企业级的分析能力。

每次运行结束后，工具会默认生成两套**同名任务目录**：

* 报告目录：`reports/upload_1mb_128t/20260224_123045/`
* 日志目录：`logs/upload_1mb_128t/20260224_123045/`

默认文件归属如下：

* `reports/<scenario_label>/<timestamp>/archive.csv`: 任务级结构化归档文件，便于后续批量汇总与自动分析。
* `reports/<scenario_label>/<timestamp>/brief.txt`: 全局配置与最终汇总报告。
* `logs/<scenario_label>/<timestamp>/realtime.txt`: 每 3 秒一次的实时采样日志；若任务在 3 秒内结束，会补写最后一条样本。
* `logs/<scenario_label>/<timestamp>/detail_X_partY.csv`: 高性能、多线程切割的请求级明细日志。

在 suite 模式下，输出目录按 suite 组织：

* `reports/<suite_id>/<timestamp>/summary/`
* `reports/<suite_id>/<timestamp>/scenarios/<scenario_id>/`
* `logs/<suite_id>/<timestamp>/scenarios/<scenario_id>/`

若使用 `--output-dir` 或 `--log-dir`，则仅替换对应类别文件的根目录，任务子目录名仍保持一致。

### 归档文件说明

#### `brief.txt`

面向人工阅读，记录：

* 本次任务的生效配置
* 最终请求统计
* CPU / RSS / TPS / BPS / 平均时延 / P99 / 单流带宽摘要

`brief.txt` 是最终摘要报告，不是中间文件。

#### `realtime.txt`

CSV 格式，列定义如下：

* `RunTime(s)`: 从任务启动到本次采样的累计时间
* `Process(%)`: 任务进度；限时任务按 `RunSeconds` 计算，定量任务按预计请求数计算
* `CPU(%)`: 相邻两次采样之间的进程 CPU 占用百分比
* `SingleCoreCPU(%)`: 相邻两次采样之间的单核等效 CPU 占用，定义为 `CPU(%) / 逻辑核数`
* `RSS(MB)`: 当前进程驻留内存
* `Interval_TPS`: 相邻两次采样之间的区间 TPS
* `Interval_BPS(Bytes/s)`: 相邻两次采样之间的区间带宽
* `Cumul_TPS`: 从任务开始到当前时刻的累计 TPS
* `Cumul_BPS(Bytes/s)`: 从任务开始到当前时刻的累计带宽
* `Peak_TPS`: 历史最高区间 TPS
* `Peak_BPS(Bytes/s)`: 历史最高区间带宽
* `Success_Rate(%)`: 当前累计成功率
* `Total_Reqs`: 当前累计请求数

控制台实时回显会对同一组指标做更易读的展示：

* `Window TPS`: 最近一个采样窗口内的请求吞吐，单位 `req/s`，控制台按四舍五入后的整数展示
* `Window BW`: 最近一个采样窗口内的带宽，基于 `streamed_bytes` 自动换算成人类可读单位
* `Avg TPS`: 从任务启动到当前采样点的累计平均请求吞吐，单位 `req/s`，控制台按四舍五入后的整数展示
* `Avg BW`: 从任务启动到当前采样点的累计平均带宽，基于 `streamed_bytes` 自动换算成人类可读单位

`realtime.txt` 是运行期采样日志，不是中间文件。

#### `archive.csv`

单任务一行，用于机器可读归档。当前字段包括：

* `task_id`（当前运行实例目录的时间戳标识，如 `20260319_211219`）
* `start_time`
* `config_file`
* `users_file`
* `op`
* `users_loaded`
* `total_threads`
* `threads_per_user_effective`
* `object_size_spec`
* `actual_duration_s`
* `total_requests`
* `success_requests`
* `failed_requests`
* `success_rate_pct`
* `avg_cpu_pct`
* `peak_cpu_pct`
* `avg_single_core_cpu_pct`
* `peak_single_core_cpu_pct`
* `avg_rss_mb`
* `peak_rss_mb`
* `final_tps`
* `peak_tps`
* `final_bps_bytes_per_sec`
* `peak_bps_bytes_per_sec`
* `avg_latency_ms`
* `p99_latency_ms`
* `avg_single_stream_bps`
* `max_single_stream_bps`
* `scenario_id`
* `git_commit`
* `host_os`
* `host_arch`

#### Suite 汇总文件

suite 模式下会额外生成：

* `suite_manifest_resolved.yaml`
* `suite_summary.csv`
* `suite_summary.json`
* `suite_summary.md`
* `suite_failures.json`
* `scenarios/<scenario_id>/longrun_summary.json`
* `scenarios/<scenario_id>/longrun_summary.md`

其中：

* `suite_summary.csv/json`：每个 scenario 一行/一项，适合横向比较四种认证场景下不同并发、PUT/GET、对象大小的性能指标，并直接给出 `perf_status`、`final_status`、`final_reason`
* `suite_summary.md`：面向人工阅读的 suite 汇总报告，带宽、时长等会用人类化单位显示
* `suite_failures.json`：记录 scenario 执行失败或长稳判定失败的场景

若启用了自动 baseline 比较，还会在具体 scenario 目录下生成：

* `perf_gate/compare.csv`
* `perf_gate/compare.md`
* `perf_gate/gate_result.json`

### 指标口径说明

为避免带宽统计失真，程序内部区分两套字节口径：

* `streamed_bytes`: 用于 `realtime.txt` 的实时区间/累计带宽采样
* `completed_success_bytes`: 仅在请求最终成功后入账，用于最终 `BPS`、`archive.csv` 和 `brief.txt` 中的汇总指标

为兼顾可读性和可机器处理性，当前采用两层展示策略：

* `archive.csv`、`suite_summary.csv/json`、`compare.csv`、`gate_result.json` 保留原始数值口径
* `brief.txt`、`suite_summary.md`、`compare.md`、`longrun_summary.md` 会把带宽、容量、时延、时长按十进制 SI 规则做人类化展示，例如 `MB/s`、`GB`、`1h 2m 3s`

主要指标定义如下：

* `Final TPS = 总请求数 / 实际运行时长`
* `Final BPS = 成功完成字节数 / 实际运行时长`
* `Avg Latency = 成功请求时延总和 / 成功请求数`
* `P99 Latency = 基于在线直方图近似统计的成功请求 P99`
* `单流带宽 = 单线程成功字节数 / 该线程实际运行时长`
* `Avg Single Stream = 所有线程单流带宽均值`
* `Max Single Stream = 所有线程单流带宽最大值`
* `Single-Core CPU = 进程 CPU(%) / 逻辑核数`

### 采样实现说明

* 采样线程默认每 3 秒运行一次
* CPU 通过 `CLOCK_PROCESS_CPUTIME_ID` 计算相邻采样窗口的区间 CPU%
* 单核等效 CPU 通过 `CPU(%) / 逻辑核数` 计算，并会同时写入 `realtime.txt`、`archive.csv` 与 suite 汇总
* 内存通过读取 `/proc/self/status` 中的 `VmRSS` 获取
* 若任务运行不足 3 秒，程序会在结束阶段强制补采样一次，因此 `realtime.txt` 仍然会有完整指标，`archive.csv` / `brief.txt` 也会得到完整汇总
* 若极端环境下无法读取 RSS，则对应字段会写为 `-1`，不会中断压测流程

### 性能基线对比与 CI 门禁

当前仓库已提供平台无关的性能对比与门禁脚本，便于后续接入 Jenkins、GitLab CI、DevCloud 等 CI 平台。

关键文件如下：

* `ci/perf/baselines.csv`: 场景到基线 `archive.csv` 的映射清单
* `ci/perf/gate_policy.json`: 默认保守门禁策略
* `ci/perf/longrun_policy.yaml`: 默认长稳分析阈值模板
* `scripts/reporting/compare_archive.py`: 对比两份 `archive.csv`
* `scripts/reporting/perf_gate.py`: 基于策略执行性能门禁
* `scripts/reporting/register_baseline.py`: 自动注册或更新某个场景的 baseline
* `scripts/reporting/analyze_longrun.py`: 基于 `realtime.txt` + `archive.csv` 分析长稳结果
* `scripts/reporting/generate_suite_summary.py`: 生成 suite 级汇总报告
* `scripts/suites/resolve_suite.py`: 解析并展开 suite YAML

手工对比示例：

```bash
python3 scripts/reporting/compare_archive.py \
  --baseline /path/to/baseline/archive.csv \
  --candidate /path/to/candidate/archive.csv \
  --output-dir ./compare_out
```

门禁执行示例：

```bash
python3 scripts/reporting/perf_gate.py \
  --candidate /path/to/candidate/archive.csv \
  --baselines-manifest ./ci/perf/baselines.csv \
  --scenario-id upload_1mb_128t \
  --policy ./ci/perf/gate_policy.json \
  --output-dir ./gate_out
```

若候选 `archive.csv` 已包含 `scenario_id`，且 `ci/perf/baselines.csv` 中存在对应记录，则脚本也支持不显式传 `--scenario-id`，直接按清单自动匹配基线。

脚本会输出：

* `compare.csv`
* `compare.md`
* `gate_result.json`

默认门禁除 `final_tps`、`final_bps_bytes_per_sec`、`avg_latency_ms`、`p99_latency_ms`、`avg_cpu_pct` 外，还会覆盖：

* `success_rate_pct`
* `failed_requests`
* `peak_tps`
* `peak_bps_bytes_per_sec`
* `peak_cpu_pct`
* `avg_rss_mb`
* `peak_rss_mb`
* `avg_single_stream_bps`
* `max_single_stream_bps`

说明：

* `avg_single_core_cpu_pct` / `peak_single_core_cpu_pct` 当前会进入归档和报告展示
* 本轮默认**不**把单核 CPU 指标纳入门禁阈值，避免额外放大回归敏感度

其中 `config_file`、`users_file`、`host_os`、`host_arch` 会作为 advisory 字段输出差异，帮助识别“环境不完全一致但仍可比较”的情况。

退出码约定：

* `0`: 门禁通过
* `1`: 检测到明显性能回退
* `2`: 输入不合法或基线/候选数据不可比

### 自动生成 Baseline 并比较

suite 模式支持两种性能基线工作模式：

* `baseline.mode: generate`
  将当前 scenario 的 `archive.csv` 自动复制到 baseline 存储目录，并自动 upsert `ci/perf/baselines.csv`
* `baseline.mode: compare`
  按 `scenario_id` 自动从 `baselines.csv` 找到对应 baseline，调用 `perf_gate.py` 比较并给出 PASS/FAIL 结论

示例：

```yaml
baseline:
  enabled: true
  mode: generate
  manifest: ./ci/perf/baselines.csv
  store_dir: ./ci/perf/baseline_archives
  policy: ./ci/perf/gate_policy.json
  update_strategy: new_label
```

后续切换为：

```yaml
baseline:
  enabled: true
  mode: compare
  manifest: ./ci/perf/baselines.csv
  store_dir: ./ci/perf/baseline_archives
  policy: ./ci/perf/gate_policy.json
  update_strategy: new_label
```

约定如下：

* `scenario_id` 是 baseline 主键，必须保持稳定
* `generate` 默认使用 `update_strategy: new_label`，保留历史 baseline 文件，并让 manifest 指向最新版本
* 若显式配置 `update_strategy: overwrite`，则会覆盖当前 `scenario_id` 对应 baseline 文件
* `compare` 会把结论写入 `perf_gate/gate_result.json`
* suite 汇总中的 `perf_status` 会显示：
  `BASELINE_UPDATED`、`PASS`、`FAIL`、`INCOMPARABLE`
* suite 汇总中的 `final_status` 会给出该场景是否满足性能要求的统一结论

典型用户路径：

1. 先用 `baseline.mode: generate` 生成一版稳定基线
2. SDK 或参数优化后，把同一 suite 切成 `baseline.mode: compare`
3. 查看 `suite_summary.md` / `suite_summary.csv` 中的 `final_status`

注意：suite 模式是“同一主进程顺序执行多个场景”，不是线程、连接或缓存态的在线热切换。

### 长稳分析

`brief.txt` 和 `archive.csv` 提供的是任务级汇总指标，但它们本身无法判断“1 小时后是否存在内存泄漏”或“吞吐是否持续衰减”。这类信息需要结合 `realtime.txt` 的时间序列来分析。

可以使用：

```bash
python3 scripts/reporting/analyze_longrun.py \
  --realtime /path/to/realtime.txt \
  --archive /path/to/archive.csv \
  --output-dir ./longrun_out \
  --policy ./ci/perf/longrun_policy.yaml
```

输出：

* `longrun_summary.json`
* `longrun_summary.md`

默认会关注：

* RSS 起点、终点、增长量、增长率和斜率
* CPU 漂移
* TPS/BPS 前段均值与后段均值的漂移
* 成功率最小值与尾段均值

若样本不足 3 条，会返回 `INSUFFICIENT_DATA`，但仍然生成分析文件。

### 一键生成分析看板

请确保系统已安装必要的 Python 库：

```bash
pip install pandas matplotlib numpy

```

**Step 1. 合并流水并计算长尾指标**

```bash
python3 scripts/reporting/merge_details.py

```

*该脚本将自动寻找最新的 task 目录，将所有线程的分片流水按绝对时间线合并为单一的 `detail.csv`，并在控制台输出平均时延与 P99 时延。*

**Step 2. 生成可视化 Dashboard**

```bash
python3 scripts/reporting/plot_report.py

```

*该脚本读取 `detail.csv`，并在对应 task 目录下生成 `dashboard.png`。图中包含：*

1. **Latency Scatter & Trend**: 请求时延散点分布与 1秒级移动平均线（检测系统抖动毛刺）。
2. **Latency CDF**: 时延累积概率分布及 P90/P99 水位线。
3. **Instant TPS & Bandwidth**: 瞬时 TPS 与吞吐带宽的双 Y 轴趋势图（检测性能掉底现象）。
4. **Status Code Distribution**: HTTP 状态码及错误分类环形占比图。

---

## ⚠️ 注意事项 (Notes)

1. **ULIMIT 限制**：在进行高并发（如 >1000 线程）压测前，请确保操作系统已提升文件句柄上限（`ulimit -n 655350`），否则由于套接字枯竭会导致大量 SDK 内部错误。
2. **网卡与带宽**：压测极致吞吐时，请监控压测机本地的网卡带宽使用率（通过 `sar -n DEV 1` 或 `nload`），避免因客户端网卡跑满导致的时延虚高。
3. **大容量日志**：长稳压测（如 7x24 小时）开启 `EnableDetailLog=true` 会占用显著的磁盘空间（尽管已实现自动轮转切割）。请确保压测机所在磁盘空间充足。 
4. **平台约束**：正式性能采样目标平台为 Linux；x86 和 ARM 均支持。若在非 Linux 环境上执行 mock，CPU/RSS 结果可能不完整。
5. **并发覆盖规则**：当使用 `--threads` 覆盖总线程数时，程序会按已加载用户数均分线程，余数依次分配给前几个用户。
6. **默认目录分离**：从当前版本开始，报告默认写入 `reports/`，日志默认写入 `logs/`；两者不会再混在同一个任务目录中。
