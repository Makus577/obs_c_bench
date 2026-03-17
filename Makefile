CC = gcc
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
PLATFORM := $(shell echo "$(UNAME_S)" | tr A-Z a-z)-$(UNAME_M)
DEFAULT_OBS_SDK_ROOT := $(CURDIR)/.deps/obs_sdk/$(PLATFORM)
OBS_SDK_ROOT ?= $(DEFAULT_OBS_SDK_ROOT)
OBS_SDK_INCLUDE := $(OBS_SDK_ROOT)/include
OBS_SDK_LIB := $(OBS_SDK_ROOT)/lib
# 基础编译选项
CFLAGS = -Wall -O2 -g -I./include -D_GNU_SOURCE -std=gnu99
# 基础链接选项
LDFLAGS = -lpthread

ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lrt
endif

# 基础目标名称
TARGET_BASE = obs_c_bench
# 默认目标名称 (可能会被下方逻辑修改)
TARGET = $(TARGET_BASE)

# 源文件列表
SRCS = src/main.c src/worker.c src/obs_adapter.c src/config_loader.c src/log.c

# -----------------------------------------------------------
# 模式控制逻辑 (修改文件名后缀)
# -----------------------------------------------------------

# 1. 检测是否开启 Mock 模式 (优先判断)
# 使用方法: make MOCK_SDK_MODE=1
ifdef MOCK_SDK_MODE
    CFLAGS += -DMOCK_SDK_MODE
    SRCS += src/mock_sdk.c
    TARGET := $(TARGET)_mock
    BUILD_TYPE_MSG += [Mock SDK Mode]
else
    # 真实 SDK 模式下链接 eSDKOBS 库
    CFLAGS += -I$(OBS_SDK_INCLUDE)
    LDFLAGS += -L$(OBS_SDK_LIB) -leSDKOBS -Wl,-rpath-link=$(OBS_SDK_LIB) -Wl,-rpath=$(OBS_SDK_LIB) -lstdc++ -lm
    BUILD_TYPE_MSG += [Real SDK Mode]
endif

# 2. 检测是否开启 ASan (AddressSanitizer)
# 使用方法: make ENABLE_ASAN=1
ifdef ENABLE_ASAN
    CFLAGS += -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS := -fsanitize=address $(LDFLAGS)
    TARGET := $(TARGET)_asan
    BUILD_TYPE_MSG += [ASan Enabled]
endif

# 生成对应的 .o 文件列表
OBJS = $(SRCS:.c=.o)

# -----------------------------------------------------------
# 构建目标
# -----------------------------------------------------------

.PHONY: all clean mock asan mock_asan clean_objs help check-sdk sdk-bootstrap

# 默认目标
all: check-sdk $(TARGET)

check-sdk:
ifndef MOCK_SDK_MODE
	@if [ ! -f "$(OBS_SDK_INCLUDE)/eSDKOBS.h" ]; then \
		echo "Missing OBS C SDK header: $(OBS_SDK_INCLUDE)/eSDKOBS.h"; \
		echo "Run 'make sdk-bootstrap' or build with OBS_SDK_ROOT=/abs/path/to/sdk make"; \
		exit 1; \
	fi
	@if [ ! -f "$(OBS_SDK_LIB)/libeSDKOBS.so" ] && [ ! -f "$(OBS_SDK_LIB)/libeSDKOBS.dylib" ] && [ ! -f "$(OBS_SDK_LIB)/libeSDKOBS.a" ]; then \
		echo "Missing OBS C SDK library under: $(OBS_SDK_LIB)"; \
		echo "Run 'make sdk-bootstrap' or build with OBS_SDK_ROOT=/abs/path/to/sdk make"; \
		exit 1; \
	fi
endif

$(TARGET): $(OBJS)
	@echo "Linking $(TARGET)..."
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "----------------------------------------"
	@echo "Build Complete: $(TARGET)"
	@echo "Mode Info: $(BUILD_TYPE_MSG)"
	@echo "----------------------------------------"
	@rm -f $(OBJS)

# 编译 .c 到 .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# -----------------------------------------------------------
# 快捷指令
# -----------------------------------------------------------

# 1. 编译 Mock 模式 -> obs_c_bench_mock
mock:
	@echo "Building Mock Mode..."
	$(MAKE) clean_objs
	$(MAKE) MOCK_SDK_MODE=1

# 2. 编译 ASan 模式 -> obs_c_bench_asan
asan:
	@echo "Building ASan Mode (Real SDK)..."
	$(MAKE) clean_objs
	$(MAKE) ENABLE_ASAN=1

# 3. 编译 Mock + ASan -> obs_c_bench_mock_asan
mock_asan:
	@echo "Building Mock + ASan Mode..."
	$(MAKE) clean_objs
	$(MAKE) MOCK_SDK_MODE=1 ENABLE_ASAN=1

sdk-bootstrap:
	@echo "Bootstrapping OBS C SDK into $(OBS_SDK_ROOT)..."
	python3 scripts/sdk/update_sdk.py --output-root "$(OBS_SDK_ROOT)"

# -----------------------------------------------------------
# 清理
# -----------------------------------------------------------

# 只清理 .o 文件
clean_objs:
	rm -f src/*.o

# 完全清理
clean: clean_objs
	rm -f $(TARGET_BASE)
	rm -f $(TARGET_BASE)_mock
	rm -f $(TARGET_BASE)_asan
	rm -f $(TARGET_BASE)_mock_asan

# 帮助信息
help:
	@echo "Available build targets:"
	@echo "  make            -> obs_c_bench           (Real SDK, Standard)"
	@echo "  make mock       -> obs_c_bench_mock      (Mock SDK)"
	@echo "  make asan       -> obs_c_bench_asan      (Real SDK + ASan)"
	@echo "  make mock_asan  -> obs_c_bench_mock_asan (Mock SDK + ASan)"
	@echo "  make sdk-bootstrap -> download/build OBS C SDK into $(DEFAULT_OBS_SDK_ROOT)"
	@echo "  make clean      -> Remove all artifacts"
	@echo "Variables:"
	@echo "  OBS_SDK_ROOT=/abs/path/to/sdk make"
