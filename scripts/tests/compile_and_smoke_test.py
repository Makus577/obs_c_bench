#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import subprocess
import os
import shutil
import time
import sys
import re
import platform

# ================= 配置区域 =================
CONFIG_FILE = 'config.dat'
CONFIG_BAK = 'config.dat.bak'
USERS_FILE = 'users.dat'
USERS_BAK = 'users.dat.bak'
CACHE_DIR = './test_bin_cache'  # 存放编译产物的临时目录
TEST_DATA_FILE = './test_data.bin' # 本地测试数据文件
TEST_DATA_SIZE_MB = 5           # 测试数据大小 (MB)
EnableDataValidation = 'false' #冒烟测试无需校验一致性

# 测试用例 ID
TEST_CASES = [201, 202, 204, 216, 230, 900]
TEST_DURATION = 0

def default_sdk_root(work_dir):
    platform_tag = f"{platform.system().lower()}-{platform.machine().lower()}"
    return os.path.join(work_dir, ".deps", "obs_sdk", platform_tag)


def build_tasks_for_env(has_real_sdk):
    tasks = [
        ("Mock", "make mock", "obs_c_bench_mock"),
        ("Mock_ASan", "make mock_asan", "obs_c_bench_mock_asan"),
    ]
    if has_real_sdk:
        tasks.insert(1, ("Standard", "make", "obs_c_bench"))
        tasks.append(("ASan", "make asan", "obs_c_bench_asan"))
    return tasks

# ===========================================

class BenchmarkTester:
    def __init__(self):
        self.results = []
        self.created_config = False
        self.created_users = False
        self.work_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        os.chdir(self.work_dir)
        self.sdk_root = os.environ.get("OBS_SDK_ROOT", default_sdk_root(self.work_dir))
        self.has_real_sdk = os.path.exists(os.path.join(self.sdk_root, "include", "eSDKOBS.h"))
        self.build_tasks = build_tasks_for_env(self.has_real_sdk)
        
        # 设置环境变量
        self.env = os.environ.copy()
        if self.has_real_sdk:
            lib_path = os.path.join(self.sdk_root, "lib")
            self.env["OBS_SDK_ROOT"] = self.sdk_root
            if 'LD_LIBRARY_PATH' in self.env:
                self.env['LD_LIBRARY_PATH'] = f"{lib_path}:{self.env['LD_LIBRARY_PATH']}"
            else:
                self.env['LD_LIBRARY_PATH'] = lib_path

    def run_cmd(self, cmd):
        """执行命令并返回 (returncode, stdout+stderr)"""
        try:
            result = subprocess.run(
                cmd, shell=True, capture_output=True, text=True, env=self.env
            )
            return result.returncode, result.stdout + result.stderr
        except Exception as e:
            return -1, str(e)

    def prepare_env(self):
        print("[Init] Preparing environment...")
        
        # 1. 创建缓存目录
        if os.path.exists(CACHE_DIR):
            shutil.rmtree(CACHE_DIR)
        os.makedirs(CACHE_DIR)

        # 2. [新增] 检查并生成测试数据文件
        if not os.path.exists(TEST_DATA_FILE):
            print(f"[Init] Generating {TEST_DATA_SIZE_MB}MB test data: {TEST_DATA_FILE} ...")
            # 使用 /dev/urandom 生成随机数据，避免压缩算法作弊
            cmd = f"dd if=/dev/urandom of={TEST_DATA_FILE} bs=1M count={TEST_DATA_SIZE_MB} status=none"
            ret, _ = self.run_cmd(cmd)
            if ret != 0:
                print(f"[Error] Failed to generate {TEST_DATA_FILE}")
                sys.exit(1)
        else:
            print(f"[Init] Test data exists: {TEST_DATA_FILE}")

        # 3. 备份并修改 Config
        if os.path.exists(CONFIG_FILE):
            shutil.copy(CONFIG_FILE, CONFIG_BAK)
        else:
            self.created_config = True
            # 创建默认 Config
            with open(CONFIG_FILE, 'w') as f:
                f.write(f"Endpoint=obs.example.com\nAK=test\nSK=test\nBucket=test\nUsers=1\nThreadsPerUser=1\nTestCase=201\nRunSeconds=3\nUploadFilePath={TEST_DATA_FILE}\n")

        if os.path.exists(USERS_FILE):
            shutil.copy(USERS_FILE, USERS_BAK)
        else:
            self.created_users = True
            with open(USERS_FILE, 'w') as f:
                f.write("user1, TEST_AK, TEST_SK\n")
        
        # 动态修改 Config: 运行时间和 UploadFilePath
        sed_cmds = [
            f"sed -i 's/^RunSeconds=.*/RunSeconds={TEST_DURATION}/g' {CONFIG_FILE}",
            # 确保 UploadFilePath 指向我们生成的文件
            f"sed -i 's|^UploadFilePath=.*|UploadFilePath={TEST_DATA_FILE}|g' {CONFIG_FILE}",
            f"sed -i 's|^EnableDataValidation=.*|EnableDataValidation={EnableDataValidation}|g' {CONFIG_FILE}"
        ]
        for cmd in sed_cmds:
            self.run_cmd(cmd)

    def restore_env(self):
        # 恢复 Config
        if os.path.exists(CONFIG_BAK):
            shutil.move(CONFIG_BAK, CONFIG_FILE)
        elif self.created_config and os.path.exists(CONFIG_FILE):
            os.remove(CONFIG_FILE)
        if os.path.exists(USERS_BAK):
            shutil.move(USERS_BAK, USERS_FILE)
        elif self.created_users and os.path.exists(USERS_FILE):
            os.remove(USERS_FILE)
        for extra_dir in ['test_reports_out', 'test_logs_out']:
            extra_path = os.path.join(self.work_dir, extra_dir)
            if os.path.exists(extra_path):
                shutil.rmtree(extra_path)
        # 清理缓存
        if os.path.exists(CACHE_DIR): shutil.rmtree(CACHE_DIR)

    def parse_stats(self, output):
        """解析 C 工具输出"""
        stats = {"success": 0, "failed": 0}
        m_failed = re.search(r"Failed:\s+(\d+)", output)
        if m_failed: stats["failed"] = int(m_failed.group(1))
        m_success = re.search(r"Success:\s+(\d+)", output)
        if m_success: stats["success"] = int(m_success.group(1))
        return stats

    def stage_compile_all(self):
        """Stage 1: 编译所有版本并缓存"""
        print("\n" + "=" * 60)
        print(">>> Stage 1: Compilation Check (Fail-Fast)")
        print("=" * 60)
        
        if not self.has_real_sdk:
            print(f"[Info] Real OBS SDK not found under {self.sdk_root}, running mock-only compilation.")

        for name, make_cmd, bin_name in self.build_tasks:
            print(f"[{name}] Compiling...", end='', flush=True)
            
            self.run_cmd("make clean")
            start_t = time.time()
            ret, output = self.run_cmd(make_cmd)
            duration = time.time() - start_t
            
            if ret != 0:
                print(f" FAIL! ({duration:.1f}s)")
                print(f"Error Log:\n{output[-1000:]}") 
                return False
            
            if not os.path.exists(bin_name):
                print(f" FAIL! (Binary {bin_name} not generated)")
                return False
            
            dst_path = os.path.join(CACHE_DIR, bin_name)
            shutil.move(bin_name, dst_path)
            os.chmod(dst_path, 0o755)
            
            print(f" PASS ({duration:.1f}s) -> Cached")
            
        print("All compilations successful.\n")
        return True

    def stage_smoke_test(self):
        """Stage 2: 使用缓存的二进制进行冒烟测试"""
        print("\n" + "=" * 60)
        print(">>> Stage 2: Smoke Testing (Mock -> Std -> Mock_ASan -> ASan)")
        print("=" * 60)
        
        for name, _, bin_name in self.build_tasks:
            bin_path = os.path.join(CACHE_DIR, bin_name)
            print(f"\n--- Testing Build: {name} ---")
            
            for case in TEST_CASES:
                print(f"  Case {case:<3} ... ", end='', flush=True)
                
                start_t = time.time()
                ret, output = self.run_cmd(f"{bin_path} {case}")
                duration = time.time() - start_t
                
                stats = self.parse_stats(output)
                status = "PASS"
                detail = ""

                # --- 判定逻辑 ---
                if ret != 0:
                    status = "FAIL"
                    detail = f"Crash(Exit {ret})"
                    if "AddressSanitizer" in output: detail = "ASan Error"
                elif "AddressSanitizer" in output:
                    status = "FAIL"
                    detail = "ASan Error (Exit 0)"
                elif stats['failed'] > 0:
                    status = "FAIL"
                    detail = f"Business Fail ({stats['failed']} errs)"
                elif stats['success'] == 0:
                    status = "WARN"
                    detail = "0 Success"
                # ----------------

                print(f"{status} (Succ:{stats['success']}, Fail:{stats['failed']}, {duration:.1f}s)")
                
                self.results.append({
                    "Build": name,
                    "Case": case,
                    "Status": status,
                    "Detail": detail
                })

    def stage_cli_archive_test(self):
        print("\n" + "=" * 60)
        print(">>> Stage 3: Output Dir Split & CLI Verification")
        print("=" * 60)

        bin_path = os.path.join(CACHE_DIR, "obs_c_bench_mock")
        existing_logs = set(d for d in os.listdir("logs") if d.startswith("task_")) if os.path.exists("logs") else set()
        existing_reports = set(d for d in os.listdir("reports") if d.startswith("task_")) if os.path.exists("reports") else set()
        custom_report_root = os.path.join(self.work_dir, "test_reports_out")
        custom_log_root = os.path.join(self.work_dir, "test_logs_out")
        if os.path.exists(custom_report_root):
            shutil.rmtree(custom_report_root)
        if os.path.exists(custom_log_root):
            shutil.rmtree(custom_log_root)

        cmd = (
            f"{bin_path} --config {CONFIG_FILE} --users {USERS_FILE} "
            f"--op upload --threads 2 --object-size 1MB "
            f"--output-dir {custom_report_root} --log-dir {custom_log_root}"
        )
        ret, output = self.run_cmd(cmd)
        if ret != 0:
            print(output)
            return False

        new_logs = sorted(set(d for d in os.listdir("logs") if d.startswith("task_")) - existing_logs)
        new_reports = sorted(set(d for d in os.listdir("reports") if d.startswith("task_")) - existing_reports)
        if new_logs:
            print("[FAIL] Default logs directory should not receive task output when --log-dir is used.")
            return False
        if new_reports:
            print("[FAIL] Default reports directory should not receive task output when --output-dir is used.")
            return False

        if not os.path.exists(custom_report_root) or not os.path.exists(custom_log_root):
            print("[FAIL] Custom output roots were not created.")
            return False

        report_tasks = sorted(d for d in os.listdir(custom_report_root) if d.startswith("task_"))
        log_tasks = sorted(d for d in os.listdir(custom_log_root) if d.startswith("task_"))
        if not report_tasks or not log_tasks:
            print("[FAIL] Missing task directories under custom report/log roots.")
            return False
        if report_tasks[-1] != log_tasks[-1]:
            print("[FAIL] Report and log task ids do not match.")
            return False

        report_dir = os.path.join(custom_report_root, report_tasks[-1])
        log_dir = os.path.join(custom_log_root, log_tasks[-1])
        archive_path = os.path.join(report_dir, "archive.csv")
        brief_path = os.path.join(report_dir, "brief.txt")
        realtime_path = os.path.join(log_dir, "realtime.txt")

        if not os.path.exists(archive_path):
            print(f"[FAIL] Missing archive.csv in {report_dir}")
            return False
        if not os.path.exists(brief_path):
            print(f"[FAIL] Missing brief.txt in {report_dir}")
            return False
        if not os.path.exists(realtime_path):
            print(f"[FAIL] Missing realtime.txt in {log_dir}")
            return False

        archive_text = open(archive_path, "r", encoding="utf-8").read()
        realtime_text = open(realtime_path, "r", encoding="utf-8").read()
        if "object_size_spec" not in archive_text or ",1MB," not in archive_text:
            print("[FAIL] archive.csv missing object size override evidence.")
            return False
        if "Interval_BPS(Bytes/s)" not in realtime_text:
            print("[FAIL] realtime.txt missing extended sampling header.")
            return False

        print(f"[PASS] Split output verification succeeded: reports={report_dir} logs={log_dir}")
        return True

    def print_summary(self):
        print("\n" + "=" * 60)
        print(f"{'BUILD':<12} | {'CASE':<6} | {'STATUS':<10} | {'DETAIL'}")
        print("-" * 60)
        
        pass_count = 0
        total_count = 0
        failed_tests = []

        for r in self.results:
            total_count += 1
            if r['Status'] == 'PASS':
                pass_count += 1
            else:
                failed_tests.append(r)
            
            print(f"{r['Build']:<12} | {r['Case']:<6} | {r['Status']:<10} | {r['Detail']}")
            
        print("-" * 60)
        print(f"Summary: {pass_count}/{total_count} Passed")
        
        if failed_tests:
            print("\nFAILED TESTS:")
            for r in failed_tests:
                print(f" - {r['Build']} Case {r['Case']}: {r['Detail']}")
            sys.exit(1)
        else:
            print("\nALL PASSED.")
            sys.exit(0)

    def run(self):
        try:
            self.prepare_env()
            if not self.stage_compile_all():
                print(">>> ABORTING: Compilation failed.")
                sys.exit(1)
            self.stage_smoke_test()
            if not self.stage_cli_archive_test():
                sys.exit(1)
            self.print_summary()
        except KeyboardInterrupt:
            print("\nInterrupted.")
        finally:
            self.restore_env()

if __name__ == "__main__":
    BenchmarkTester().run()
