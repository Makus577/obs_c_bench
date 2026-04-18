# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`obs_c_bench` is a high-performance C benchmark tool for Huawei Cloud OBS (Object Storage Service). It measures TPS, bandwidth, and latency using the official OBS C SDK with a lock-free, multi-threaded architecture.

## Build Commands

```bash
# Build real SDK version
make

# Build mock version (no network, for local logic testing)
make mock

# Build with AddressSanitizer
make asan

# Build mock + ASAN
make mock_asan

# Download and bootstrap OBS C SDK
make sdk-bootstrap

# Clean artifacts
make clean

# Smoke test (builds all variants)
python3 scripts/tests/compile_and_smoke_test.py
```

## Core Architecture

### Threading Model
- **Worker threads**: Execute benchmark requests and collect per-thread statistics using atomic counters
- **Monitor thread**: Runs every 3 seconds to sample global CPU/RSS/TPS/bandwidth metrics
- **Lock-free design**: No locks in worker hot path; metrics use `stdatomic` operations

### Data Flow
```
main.c → config_loader.c → worker threads → obs_adapter.c → OBS SDK
                                                    ↓
                              (callback-driven: data_handler, properties_handler, completion_handler)
```

### Key Abstractions
- `WorkerArgs`: Per-thread state bundle (config, pattern buffer, stats, thread index)
- `transfer_context`: Carries operation state through OBS SDK callbacks
- `TestCase` values: 201=PUT, 202=GET, 204=DELETE, 216=MULTIPART, 230=RESUMABLE, 900=MIX
- `ObjNamePatternHash=true`: Enables deterministic LCG-based object naming to avoid storage hotspots

### Configuration Priority
CLI args > `scenario.yaml` / `simple_config.yaml` > `config.dat` (profile) > defaults

## Key Files

| File | Role |
|------|------|
| `src/main.c` | Entry point, config parsing, monitor thread setup |
| `src/worker.c` | Worker thread execution, latency tracking, atomic stats |
| `src/obs_adapter.c` | Wraps OBS SDK calls (PUT/GET/DELETE/MULTIPART/RESUMABLE) with callbacks |
| `src/mock_sdk.c` | Fake SDK implementation used when `MOCK_SDK_MODE=1` |
| `src/config_loader.c` | Parses `config.dat`, `scenario.yaml`, CLI arguments |
| `src/log.c` | Per-thread log rotation (1M lines per file) |
| `include/mock_eSDKOBS.h` | Mock OBS SDK API matching real SDK interface |
| `scripts/reporting/merge_details.py` | Merge worker CSVs, compute P99/P99.9 latency |
| `scripts/reporting/plot_report.py` | Generate 2x2 dashboard PNGs |

## Important Patterns

### Adding a New Test Case
Test cases are driven by `TestCase` enum values. To add case 250 (e.g., HEAD request):
1. Define the case value in config with `TestCase=250`
2. In `obs_adapter.c`, add a case branch in the upload/download handler that calls the appropriate OBS SDK function
3. The mock SDK (`mock_sdk.c`) should also implement the corresponding mock operation

### Data Validation
When `EnableDataValidation=true`, a 1MB deterministic pattern buffer is pre-allocated per thread. On PUT, data is generated via `put_buffer_callback_optimized`. On GET, `get_buffer_callback_optimized` validates downloaded data against expected pattern using absolute offsets.

### Bucket Routing
Multi-user mode routes traffic using `users.dat` (AK/SK pairs). Bucket names are auto-generated as `{ak_lowercase}.{BucketNamePrefix}`, or use `BucketNameFixed` for a single bucket.

## Known Concerns

- **Memory**: Multipart etag `strdup()` freed only on error path
- **Security**: AK/SK stored in plaintext in `users.dat`
- **Signal safety**: `volatile sig_atomic_t` used with non-async-signal-safe functions
- **Memory scaling**: Fixed 1MB pattern buffer per thread; with many threads, memory can be significant

## Development Workflow

1. **Local logic testing**: `make mock && ./obs_c_bench_mock --config ./scenario.yaml`
2. **Memory issues**: `make mock_asan && ./obs_c_bench_mock_asan --config ./scenario.yaml`
3. **Real OBS testing**: `make sdk-bootstrap && make && ./obs_c_bench --config ./scenario.yaml`
4. **Performance baseline**: Use suite mode with `baseline.mode: generate` then `compare`

## Config Examples

```yaml
# scenario.yaml - minimal upload test
profile: ./config.dat
users: ./users.dat
op: upload
threads: 128
object_size: 1MB
requests_per_thread: 100
scenario_id: upload_1mb_128t
```

## Reporting Outputs

- `reports/<scenario>/<timestamp>/archive.csv` - Machine-readable summary
- `reports/<scenario>/<timestamp>/brief.txt` - Human-readable report
- `logs/<scenario>/<timestamp>/realtime.txt` - 3-second sampling data
- `logs/<scenario>/<timestamp>/detail_*.csv` - Per-request logs

<!-- GSD:project-start source:PROJECT.md -->
## Project

**obs_c_bench 增强：长稳测试与场景对比**

`obs_c_bench` 是一款华为云 OBS C SDK 工业级压测工具。当前已具备 Suite 模式、长稳分析、Baseline 对比能力。本项目聚焦于三个核心增强：**简化场景配置体验**、**全流程一键串联**、**长稳分析增强**，让压测工具更易于使用和维护。

**Core Value:** 让 performance benchmarking 从「配置复杂」到「一键可复现」，建立可持续的性能追踪体系。

### Constraints

- **向后兼容**: 现有 `config.dat`、`scenario.yaml` 格式必须继续支持，不能破坏已有工作流
- **一键体验**: 增强不能引入新的复杂依赖，所有脚本在 `python3` 标准库 + 已有 pip 依赖下可运行
- **性能开销**: 报告生成和分析脚本本身的执行时间应 < 压测时间的 5%
<!-- GSD:project-end -->

<!-- GSD:stack-start source:codebase/STACK.md -->
## Technology Stack

## Languages
- **C** (gnu99 standard) - Core benchmark engine, worker threads, SDK adapter, configuration loader
- **Python 3** - Build辅助 scripts, SDK bootstrap, credential generation, reporting
## Compiler & Build System
- **GCC** - Primary C compiler
- Standard: `-std=gnu99` (POSIX/GNU extensions enabled)
- Flags: `-Wall -O2 -g -D_GNU_SOURCE`
- **GNU Make** - Primary build orchestration
- Makefile handles multiple build modes:
## Key Dependencies
- **pthread** - POSIX threads for concurrent worker threads and monitoring
- **libc** - Standard C library (memcpy, malloc, fprintf, etc.)
- **eSDKOBS** (Huawei Cloud OBS C SDK) - Real OBS operations
| Library | Version | Purpose | Location |
|---------|---------|---------|----------|
| **cJSON** | 1.7.18 | JSON parsing for config/reporting | `include/cJSON.h` |
| **SecureC** | Huawei internal | Safe string functions (memcpy_s, strcpy_s, etc.) | `include/securec.h` |
- `urllib.request` - SDK download
- `zipfile` - SDK extraction
- `subprocess` - Build command execution
- `requests` - IAM token acquisition
## Runtime Environment
- **Linux** (primary) - Full feature support, `librt` additional linking
- **macOS** (Darwin) - Supported via uname detection
- **Platform detection:** `uname -s` and `uname -m` for cross-platform SDK paths
- **Multi-threaded** - One worker thread per configured thread, plus monitor thread
- **Signal handling** - SIGINT graceful shutdown, SIGPIPE ignored
- **Process monitoring** - Reads `/proc/self/status` for RSS memory on Linux
- `obs_initialize(OBS_INIT_ALL)` at startup
- `obs_deinitialize()` at cleanup
## Data Patterns
- LCG (Linear Congruential Generator) for deterministic test data
- Formula: `buf[i] = (i * 1103515245 + 12345) % 2^31`
- Buffer size: 1MB pattern cycling
- Used for both PUT payload generation and GET validation
- 371 buckets for latency distribution (0ms to 10000ms+)
- P99 calculation via cumulative histogram
## Build Variants
| Variant | Binary Suffix | Notes |
|---------|--------------|-------|
| Standard | `obs_c_bench` | Real SDK, optimized |
| Mock | `obs_c_bench_mock` | Mock SDK, no network |
| ASan | `obs_c_bench_asan` | AddressSanitizer enabled |
| Mock+ASan | `obs_c_bench_mock_asan` | Combined |
## Configuration
- Endpoint, Protocol (http/https)
- Threads, Users, ObjectSize
- PartSize, Range
- GmAuthMode (国密认证 modes)
- Certificate paths for TLS
<!-- GSD:stack-end -->

<!-- GSD:conventions-start source:CONVENTIONS.md -->
## Conventions

## Language
## C Source Organization
### File Structure
### Header Inclusion
#include "bench.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <getopt.h>
#include <sys/utsname.h>
#include <sys/wait.h>
## Naming Conventions
### Files
- C source files use lowercase with underscores: `config_loader.c`, `obs_adapter.c`
- Header files use lowercase: `bench.h`, `log.h`
- Python scripts use underscores: `compile_and_smoke_test.py`, `merge_details.py`
### Variables
### Functions
| Prefix | Purpose | Example |
|--------|---------|---------|
| `worker_` | Worker thread operations | `worker_routine()` |
| `obs_` | OBS SDK adapter layer | `obs_initialize()`, `obs_deinitialize()` |
| `run_` | Benchmark execution | `run_put_benchmark()`, `run_multipart_benchmark()` |
| `load_` | Configuration loading | `load_config()`, `load_users_file()` |
| `format_` | Output formatting | `format_rate_si()`, `format_bytes_si()` |
| `parse_` | Parsing operations | `parse_cli_options()`, `parse_object_size_spec()` |
| `ensure_` | Directory/file utilities | `ensure_directory()` |
| `log_` | Logging subsystem | `log_init()`, `LOG_INFO()` |
### Types and Enums
### Constants and Macros
#define PATTERN_BUF_SIZE (1 * 1024 * 1024)
#define MAX_ROWS_PER_FILE 1000000
#define BATCH_SIZE 64
## Code Style
### Braces and Indentation
### Control Flow
### Memory Allocation
### Error Handling
## Threading Model
### Thread-Specific Data
### Atomic Operations
### Signal Handling
## String Handling
## Logging
## Python Style
### Shebang and Encoding
#!/usr/bin/env python3
### Docstrings
### Imports
### Test Organization (pytest)
## Build System
| Target | Output | Purpose |
|--------|--------|---------|
| `make` | `obs_c_bench` | Real SDK, standard build |
| `make mock` | `obs_c_bench_mock` | Mock SDK for local testing |
| `make asan` | `obs_c_bench_asan` | Real SDK with AddressSanitizer |
| `make mock_asan` | `obs_c_bench_mock_asan` | Mock + ASAN |
## Documentation
<!-- GSD:conventions-end -->

<!-- GSD:architecture-start source:ARCHITECTURE.md -->
## Architecture

## Pattern Overview
- Callback-based async SDK interface (all SDK operations use callbacks for data/properties/completion)
- pthread-based worker pool with a dedicated monitor thread for metrics collection
- Dual-mode operation: real OBS SDK vs. in-process mock SDK
- Time-bounded or request-bounded execution modes
- Per-request detail logging with batched file writes (BATCH_SIZE=1000)
## Layers
- Purpose: Parse command-line arguments, select execution mode (single vs. suite)
- Location: `src/main.c` (lines 2131-2162)
- Contains: `main()`, `parse_cli_options()`, `handle_sigint()`
- Depends on: config_loader, worker, monitor
- Used by: Shell invocation
- Purpose: Load and validate benchmark configuration from file or CLI
- Location: `src/config_loader.c`, `src/main.c` (run_benchmark_scenario)
- Contains: `load_config()`, `load_users_file()`, `parse_object_size_spec()`, config validation
- Depends on: `bench.h` (Config struct definition)
- Used by: main, worker
- Purpose: Execute benchmark operations in worker threads
- Location: `src/worker.c`, `src/obs_adapter.c`
- Contains: `worker_routine()`, `run_put_benchmark()`, `run_get_benchmark()`, `run_multipart_benchmark()`, `run_upload_file_benchmark()`
- Depends on: SDK (real or mock), log
- Used by: main (via pthread_create)
- Purpose: Wrap the OBS SDK (real or mock) with consistent callbacks for data, properties, and completion
- Location: `src/obs_adapter.c`
- Contains: `setup_options()`, `put_buffer_callback_optimized()`, `get_buffer_callback_optimized()`, `transfer_context`
- Depends on: `include/mock_eSDKObs.h` (interface definition), `bench.h`
- Used by: worker
- Purpose: In-process mock implementation of OBS SDK for testing without cloud backend
- Location: `src/mock_sdk.c`
- Contains: Mock implementations of `put_object()`, `get_object()`, `delete_object()`, `list_bucket_objects()`, `initiate_multi_part_upload()`, `upload_part()`, `complete_multi_part_upload()`, `upload_file()`
- Depends on: `include/mock_eSDKOBS.h`
- Used by: (linked in mock mode only, replaces real SDK)
- Purpose: Collect and report real-time performance metrics
- Location: `src/main.c` (monitor_routine lines 872-903, emit_monitor_snapshot lines 764-870)
- Contains: `MonitorArgs`, `monitor_routine()`, `emit_monitor_snapshot()`
- Depends on: worker stats aggregation
- Used by: main (via pthread_create)
- Purpose: Thread-safe structured logging with per-thread context
- Location: `src/log.c`, `include/log.h`
- Contains: `log_init()`, `log_set_context()`, `log_message()`, LOG_DEBUG/INFO/WARN/ERROR macros
- Used by: All layers
## Data Flow
## Key Abstractions
- Purpose: Carries operation state through SDK callbacks
- Contains: WorkerArgs pointer, bytes processed, validation state, returned upload_id/etag/request_id, pattern offset for validation
- This is the core state machine for each in-flight operation
- Purpose: Per-thread argument bundle
- Contains: thread_id, config pointer, stats structure, pattern buffer, effective AK/SK/token/bucket, timing
- Purpose: Per-thread counter aggregation
- Contains: success/fail counts by HTTP code, streamed bytes, latency sum/min/max, latency histogram (371 buckets)
- Purpose: Global benchmark configuration
- Contains: endpoint, credentials, thread count, object size, part size, timeouts, SSL/TLS settings, range options, MIX mode ops
- Purpose: Defines the OBS SDK API surface that both real SDK and mock must implement
- Contains: obs_status enum (170+ values), obs_options, obs_bucket_context, obs_http_request_option, put/get/delete/list/upload_part/complete_multi_part_upload/upload_file functions, callback types
## Entry Points
- Triggers: Direct binary invocation or `make` run
- Responsibilities: CLI parsing, signal setup, mode selection (single/suite), directory creation, calls run_benchmark_scenario() or run_suite_mode()
- Triggers: pthread_create() from main
- Responsibilities: Per-thread benchmark loop, operation dispatch, stat updates, detail logging
- Triggers: pthread_create() from main after workers spawned
- Responsibilities: Periodic metrics collection, real-time reporting, graceful shutdown coordination
## Error Handling
- SDK errors classified by HTTP status code (403/404/409/5xx/other) in `infer_http_code()` (worker.c lines 17-50)
- Validation failures tracked separately (`fail_validation_count`)
- On SDK failure, 50ms sleep before retry (`usleep(50000)` in worker.c line 245)
- Global `g_graceful_stop` flag checked at each operation boundary for clean shutdown
- Double SIGINT handling: first triggers graceful stop, second force-exits
- Access failures (403): Credentials or permission issues
- Not found (404): Object/bucket does not exist
- Conflict (409): Bucket already exists, concurrent operations
- Server errors (5xx): Backend issues
- Network errors (0 code): Curl failures, timeouts
- Validation failures: Data corruption detected by checksum comparison
## Cross-Cutting Concerns
<!-- GSD:architecture-end -->

<!-- GSD:skills-start source:skills/ -->
## Project Skills

No project skills found. Add skills to any of: `.claude/skills/`, `.agents/skills/`, `.cursor/skills/`, or `.github/skills/` with a `SKILL.md` index file.
<!-- GSD:skills-end -->

<!-- GSD:workflow-start source:GSD defaults -->
## GSD Workflow Enforcement

Before using Edit, Write, or other file-changing tools, start work through a GSD command so planning artifacts and execution context stay in sync.

Use these entry points:
- `/gsd-quick` for small fixes, doc updates, and ad-hoc tasks
- `/gsd-debug` for investigation and bug fixing
- `/gsd-execute-phase` for planned phase work

Do not make direct repo edits outside a GSD workflow unless the user explicitly asks to bypass it.
<!-- GSD:workflow-end -->

<!-- GSD:profile-start -->
## Developer Profile

> Profile not yet configured. Run `/gsd-profile-user` to generate your developer profile.
> This section is managed by `generate-claude-profile` -- do not edit manually.
<!-- GSD:profile-end -->
