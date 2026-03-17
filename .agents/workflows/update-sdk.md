---
description: 自动化下载/解析最新的华为云 OBS C SDK 并重新编译本工具。
---

本项目提供了一个自动化脚本 `scripts/sdk/update_sdk.py`，用于下载并构建依赖的 SDK 到本地缓存目录。

// turbo
1. 执行更新脚本：
```bash
python3 scripts/sdk/update_sdk.py
```

### 说明
- **架构自动识别**：脚本会自动检测当前系统是 x86_64 还是 aarch64，并调用相应的上游构建脚本。
- **本地缓存输出**：默认将构建产物输出到 `.deps/obs_sdk/<platform>/`，不会覆盖仓库源码树中的文件。
- **后续构建**：脚本成功后会打印 `OBS_SDK_ROOT` 和下一步 `make` 命令，供真实 SDK 模式编译使用。
