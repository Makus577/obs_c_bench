# OBS C SDK Benchmark Tool (obs_c_bench)

`obs_c_bench` 是一款专为华为云对象存储服务 (OBS) 打造的工业级、高性能 C 语言压测工具。它基于官方 OBS C SDK 开发，采用了极致的无锁并发架构，旨在帮助开发者、架构师和测试工程师准确评估云存储后端的极限 TPS、吞吐带宽以及长尾时延表现。

当前版本已支持：

* 通过 CLI 显式指定 `config.dat` / `users.dat`
* 用命令行覆盖操作类型、总并发数、对象大小
* 通过 CLI 分别指定报告导出目录和日志目录
* 在 Linux x86 / ARM 环境下采集 CPU、RSS、TPS、BPS、平均时延、P99 时延、单流带宽
* 默认将报告输出到 `reports/`，日志输出到 `logs/`
* 通过 `make sdk-bootstrap` 或 `OBS_SDK_ROOT=/path make` 显式引入真实 OBS C SDK

## 🌟 核心特性 (Key Features)

* **极致的并发性能 (Lock-Free Architecture)**
* Worker 线程执行请求及本地统计数据收集时**全程无锁**，榨干压测机每一滴 CPU 性能。
* 独立的旁路监控线程（Monitor Thread）每 3 秒无锁采集全局状态，实时输出 CPU、内存、TPS、BPS 与成功率；若任务在 3 秒内结束，会自动补写最终样本，避免短任务无采样数据。


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
* 工具自动按任务时间戳创建独立隔离目录（如 `reports/task_20260224_120000/` 与 `logs/task_20260224_120000/`）。
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
* Python 3.x 及 `pandas`, `matplotlib` (仅用于后期图表生成)

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

*(注：详细配置说明请参考 `config.dat` 文件内的中文注释)*

### 3. 执行压测

工具默认读取当前目录下的 `config.dat`：

```bash
./obs_c_bench

```

默认输出目录会天然分离：

* 报告目录：`reports/task_<timestamp>/`
* 日志目录：`logs/task_<timestamp>/`

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

### 4. CLI 参数说明

| 参数 | 说明 | 示例 |
| --- | --- | --- |
| `--config <path>` | 指定配置文件路径，默认 `config.dat` | `--config ./conf/config.dat` |
| `--users <path>` | 指定用户文件路径，默认 `users.dat` | `--users ./accounts/users.dat` |
| `--op <value>` | 覆盖操作类型，支持 `upload/download/delete/multipart/resumable/mix` 或 `201/202/204/216/230/900` | `--op upload` |
| `--threads <N>` | 覆盖总并发线程数；多用户时会按用户均分，余数从前往后补齐 | `--threads 1024` |
| `--object-size <spec>` | 覆盖对象大小，支持纯字节数、十进制单位和区间 | `--object-size 1MB~16MB` |
| `--output-dir <path>` | 仅控制报告导出目录，作用于 `archive.csv`、`brief.txt` | `--output-dir /data/reports` |
| `--log-dir <path>` | 仅控制日志目录，作用于 `realtime.txt`、`detail_*.csv` | `--log-dir /data/logs` |

参数优先级固定为：`CLI > config.dat > 默认值`。

### 5. 对象大小写法

`ObjectSize` 与 `--object-size` 采用相同语法：

* 固定大小：`4096`
* 十进制单位：`512KB`、`1MB`、`2GB`
* 范围写法：`1024~4096`、`1MB~16MB`

当前单位按十进制换算：

* `1KB = 1000 Bytes`
* `1MB = 1000 * 1000 Bytes`
* `1GB = 1000 * 1000 * 1000 Bytes`

---

## 📊 日志与数据可视化 (Logging & Visualization)

当您在 `config.dat` 中配置了 `EnableDetailLog=true` 后，工具将为您提供企业级的分析能力。

每次运行结束后，工具会默认生成两套**同名任务目录**：

* 报告目录：`reports/task_20260224_123045/`
* 日志目录：`logs/task_20260224_123045/`

默认文件归属如下：

* `reports/task_xxx/archive.csv`: 任务级结构化归档文件，便于后续批量汇总与自动分析。
* `reports/task_xxx/brief.txt`: 全局配置与最终汇总报告。
* `logs/task_xxx/realtime.txt`: 每 3 秒一次的实时采样日志；若任务在 3 秒内结束，会补写最后一条样本。
* `logs/task_xxx/detail_X_partY.csv`: 高性能、多线程切割的请求级明细日志。

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
* `RSS(MB)`: 当前进程驻留内存
* `Interval_TPS`: 相邻两次采样之间的区间 TPS
* `Interval_BPS(Bytes/s)`: 相邻两次采样之间的区间带宽
* `Cumul_TPS`: 从任务开始到当前时刻的累计 TPS
* `Cumul_BPS(Bytes/s)`: 从任务开始到当前时刻的累计带宽
* `Peak_TPS`: 历史最高区间 TPS
* `Peak_BPS(Bytes/s)`: 历史最高区间带宽
* `Success_Rate(%)`: 当前累计成功率
* `Total_Reqs`: 当前累计请求数

`realtime.txt` 是运行期采样日志，不是中间文件。

#### `archive.csv`

单任务一行，用于机器可读归档。当前字段包括：

* `task_id`
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

### 指标口径说明

为避免带宽统计失真，程序内部区分两套字节口径：

* `streamed_bytes`: 用于 `realtime.txt` 的实时区间/累计带宽采样
* `completed_success_bytes`: 仅在请求最终成功后入账，用于最终 `BPS`、`archive.csv` 和 `brief.txt` 中的汇总指标

主要指标定义如下：

* `Final TPS = 总请求数 / 实际运行时长`
* `Final BPS = 成功完成字节数 / 实际运行时长`
* `Avg Latency = 成功请求时延总和 / 成功请求数`
* `P99 Latency = 基于在线直方图近似统计的成功请求 P99`
* `单流带宽 = 单线程成功字节数 / 该线程实际运行时长`
* `Avg Single Stream = 所有线程单流带宽均值`
* `Max Single Stream = 所有线程单流带宽最大值`

### 采样实现说明

* 采样线程默认每 3 秒运行一次
* CPU 通过 `CLOCK_PROCESS_CPUTIME_ID` 计算相邻采样窗口的区间 CPU%
* 内存通过读取 `/proc/self/status` 中的 `VmRSS` 获取
* 若任务运行不足 3 秒，程序会在结束阶段强制补采样一次，因此 `realtime.txt` 仍然会有完整指标，`archive.csv` / `brief.txt` 也会得到完整汇总
* 若极端环境下无法读取 RSS，则对应字段会写为 `-1`，不会中断压测流程

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
