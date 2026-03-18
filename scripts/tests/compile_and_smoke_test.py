#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import subprocess
import os
import shutil
import time
import sys
import re
import platform
import json

# ================= 配置区域 =================
CONFIG_FILE = 'config.dat'
CONFIG_BAK = 'config.dat.bak'
USERS_FILE = 'users.dat'
USERS_BAK = 'users.dat.bak'
CACHE_DIR = './test_bin_cache'  # 存放编译产物的临时目录
TEST_DATA_FILE = './test_data.bin' # 本地测试数据文件
TEST_DATA_SIZE_MB = 5           # 测试数据大小 (MB)
EnableDataValidation = 'false' #冒烟测试无需校验一致性
SUITE_FILE = './test_suite.yaml'
PROFILE_CONFIG_A = './test_profile_a.dat'
PROFILE_CONFIG_B = './test_profile_b.dat'

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
        for extra_dir in ['test_reports_out', 'test_logs_out', 'test_perf_gate_out', 'test_suite_reports', 'test_suite_logs', 'test_auto_baseline']:
            extra_path = os.path.join(self.work_dir, extra_dir)
            if os.path.exists(extra_path):
                shutil.rmtree(extra_path)
        for extra_file in [SUITE_FILE, PROFILE_CONFIG_A, PROFILE_CONFIG_B]:
            extra_path = os.path.join(self.work_dir, extra_file)
            if os.path.exists(extra_path):
                os.remove(extra_path)
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
            f"--op upload --threads 2 --object-size 1MB --scenario-id smoke_upload_1mb_2t "
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
        brief_text = open(brief_path, "r", encoding="utf-8").read()
        realtime_text = open(realtime_path, "r", encoding="utf-8").read()
        if "object_size_spec" not in archive_text or ",1MB," not in archive_text:
            print("[FAIL] archive.csv missing object size override evidence.")
            return False
        if "scenario_id" not in archive_text or "smoke_upload_1mb_2t" not in archive_text:
            print("[FAIL] archive.csv missing scenario_id metadata.")
            return False
        if "avg_single_core_cpu_pct" not in archive_text or "peak_single_core_cpu_pct" not in archive_text:
            print("[FAIL] archive.csv missing single-core CPU columns.")
            return False
        if "Interval_BPS(Bytes/s)" not in realtime_text:
            print("[FAIL] realtime.txt missing extended sampling header.")
            return False
        if "SingleCoreCPU(%)" not in realtime_text:
            print("[FAIL] realtime.txt missing SingleCoreCPU(%) header.")
            return False
        if "MB/s" not in brief_text and "KB/s" not in brief_text and "Bytes/s" not in brief_text:
            print("[FAIL] brief.txt missing human-readable rate unit.")
            return False

        print(f"[PASS] Split output verification succeeded: reports={report_dir} logs={log_dir}")
        return True

    def stage_perf_gate_test(self):
        print("\n" + "=" * 60)
        print(">>> Stage 4: Perf Baseline Comparison & Gate Verification")
        print("=" * 60)

        perf_root = os.path.join(self.work_dir, "test_perf_gate_out")
        if os.path.exists(perf_root):
            shutil.rmtree(perf_root)
        os.makedirs(perf_root)

        fixtures_dir = os.path.join(self.work_dir, "ci", "perf", "fixtures")
        manifest_path = os.path.join(self.work_dir, "ci", "perf", "baselines.csv")
        policy_path = os.path.join(self.work_dir, "ci", "perf", "gate_policy.json")
        baseline_path = os.path.join(fixtures_dir, "baseline_upload_1mb_128t.csv")
        better_path = os.path.join(fixtures_dir, "candidate_better_upload_1mb_128t.csv")
        worse_path = os.path.join(fixtures_dir, "candidate_worse_upload_1mb_128t.csv")
        failures_path = os.path.join(fixtures_dir, "candidate_failures_upload_1mb_128t.csv")
        mismatch_path = os.path.join(fixtures_dir, "candidate_mismatch_upload_1mb_256t.csv")

        compare_dir = os.path.join(perf_root, "compare")
        ret, output = self.run_cmd(
            f"python3 scripts/reporting/compare_archive.py --baseline {baseline_path} "
            f"--candidate {better_path} --output-dir {compare_dir}"
        )
        if ret != 0:
            print(output)
            print("[FAIL] compare_archive.py should succeed for comparable archives.")
            return False
        if not os.path.exists(os.path.join(compare_dir, "compare.csv")):
            print("[FAIL] compare_archive.py did not produce compare.csv")
            return False
        if not os.path.exists(os.path.join(compare_dir, "compare.md")):
            print("[FAIL] compare_archive.py did not produce compare.md")
            return False
        compare_md = open(os.path.join(compare_dir, "compare.md"), "r", encoding="utf-8").read()
        if "/s" not in compare_md:
            print("[FAIL] compare.md missing human-readable throughput units.")
            return False

        pass_dir = os.path.join(perf_root, "gate_pass")
        ret, output = self.run_cmd(
            f"python3 scripts/reporting/perf_gate.py --candidate {better_path} "
            f"--policy {policy_path} --baselines-manifest {manifest_path} "
            f"--output-dir {pass_dir}"
        )
        if ret != 0:
            print(output)
            print("[FAIL] perf_gate.py should pass for improved candidate via manifest auto-match.")
            return False
        with open(os.path.join(pass_dir, "gate_result.json"), "r", encoding="utf-8") as handle:
            pass_result = json.load(handle)
        if pass_result.get("overall_status") != "PASS":
            print("[FAIL] perf_gate.py pass case did not report PASS.")
            return False

        failure_dir = os.path.join(perf_root, "gate_failures")
        ret, output = self.run_cmd(
            f"python3 scripts/reporting/perf_gate.py --baseline {baseline_path} "
            f"--candidate {failures_path} --policy {policy_path} --output-dir {failure_dir}"
        )
        if ret != 1:
            print(output)
            print("[FAIL] perf_gate.py should fail when candidate introduces failed requests.")
            return False
        with open(os.path.join(failure_dir, "gate_result.json"), "r", encoding="utf-8") as handle:
            failure_result = json.load(handle)
        if "failed_requests" not in failure_result.get("failed_metrics", []):
            print("[FAIL] perf_gate.py did not flag failed_requests regression.")
            return False

        fail_dir = os.path.join(perf_root, "gate_fail")
        ret, output = self.run_cmd(
            f"python3 scripts/reporting/perf_gate.py --baseline {baseline_path} "
            f"--candidate {worse_path} --policy {policy_path} --output-dir {fail_dir}"
        )
        if ret != 1:
            print(output)
            print("[FAIL] perf_gate.py should return exit code 1 for regressed candidate.")
            return False
        with open(os.path.join(fail_dir, "gate_result.json"), "r", encoding="utf-8") as handle:
            fail_result = json.load(handle)
        if fail_result.get("overall_status") != "FAIL":
            print("[FAIL] perf_gate.py fail case did not report FAIL.")
            return False

        mismatch_dir = os.path.join(perf_root, "gate_mismatch")
        ret, output = self.run_cmd(
            f"python3 scripts/reporting/perf_gate.py --baseline {baseline_path} "
            f"--candidate {mismatch_path} --policy {policy_path} --output-dir {mismatch_dir}"
        )
        if ret != 2:
            print(output)
            print("[FAIL] perf_gate.py should return exit code 2 for incomparable archives.")
            return False
        with open(os.path.join(mismatch_dir, "gate_result.json"), "r", encoding="utf-8") as handle:
            mismatch_result = json.load(handle)
        if mismatch_result.get("overall_status") != "INCOMPARABLE":
            print("[FAIL] perf_gate.py mismatch case did not report INCOMPARABLE.")
            return False

        print(f"[PASS] Perf gate verification succeeded under {perf_root}")
        return True

    def stage_suite_test(self):
        print("\n" + "=" * 60)
        print(">>> Stage 5: Suite Mode & Long-Run Artifact Verification")
        print("=" * 60)

        bin_path = os.path.join(CACHE_DIR, "obs_c_bench_mock")
        suite_report_root = os.path.join(self.work_dir, "test_suite_reports")
        suite_log_root = os.path.join(self.work_dir, "test_suite_logs")
        profile_a = os.path.join(self.work_dir, PROFILE_CONFIG_A)
        profile_b = os.path.join(self.work_dir, PROFILE_CONFIG_B)
        suite_path = os.path.join(self.work_dir, SUITE_FILE)

        shutil.copy(CONFIG_FILE, profile_a)
        shutil.copy(CONFIG_FILE, profile_b)

        suite_yaml = f"""suite_id: smoke_suite
description: mock suite verification
defaults:
  users_file: {USERS_FILE}
  requests_per_thread: 2
profiles:
  inter_oneway:
    config_file: {PROFILE_CONFIG_A}
  gm_oneway:
    config_file: {PROFILE_CONFIG_B}
matrix:
  profile: [inter_oneway, gm_oneway]
  op: [upload]
  threads: [2]
  object_size: [1MB]
scenarios:
  - scenario_id: suite_download_custom
    profile: inter_oneway
    op: download
    threads: 1
    object_size: 512KB
    analyze_longrun: true
gates:
  longrun:
    enabled: false
    fail_on_regression: false
    policy: ci/perf/longrun_policy.yaml
reporting:
  continue_on_fail: true
"""
        with open(suite_path, "w", encoding="utf-8") as handle:
            handle.write(suite_yaml)

        ret, output = self.run_cmd(
            f"{bin_path} --suite {suite_path} --output-dir {suite_report_root} --log-dir {suite_log_root}"
        )
        if ret != 0:
            print(output)
            print("[FAIL] Suite mode should succeed in mock environment.")
            return False

        suite_root = os.path.join(suite_report_root, "smoke_suite")
        if not os.path.exists(suite_root):
            print("[FAIL] Missing suite report root.")
            return False
        run_dirs = sorted(os.listdir(suite_root))
        if not run_dirs:
            print("[FAIL] Missing suite run directories.")
            return False
        run_dir = os.path.join(suite_root, run_dirs[-1])
        summary_dir = os.path.join(run_dir, "summary")
        suite_log_run_dir = os.path.join(suite_log_root, "smoke_suite", run_dirs[-1], "scenarios")

        expected_summary_files = [
            "suite_manifest_resolved.yaml",
            "suite_summary.csv",
            "suite_summary.json",
            "suite_summary.md",
            "suite_failures.json",
        ]
        for filename in expected_summary_files:
            if not os.path.exists(os.path.join(summary_dir, filename)):
                print(f"[FAIL] Missing suite summary artifact: {filename}")
                return False

        scenario_dir = os.path.join(run_dir, "scenarios", "suite_download_custom")
        if not os.path.exists(os.path.join(scenario_dir, "archive.csv")):
            print("[FAIL] Missing scenario archive.csv under suite output.")
            return False
        if not os.path.exists(os.path.join(scenario_dir, "longrun_summary.json")):
            print("[FAIL] Missing longrun_summary.json for analyzed scenario.")
            return False
        if not os.path.exists(os.path.join(suite_log_run_dir, "suite_download_custom", "realtime.txt")):
            print("[FAIL] Missing suite scenario realtime.txt.")
            return False

        with open(os.path.join(summary_dir, "suite_summary.csv"), "r", encoding="utf-8") as handle:
            summary_text = handle.read()
        if "suite_download_custom" not in summary_text or "smoke_suite" not in summary_text:
            print("[FAIL] suite_summary.csv missing scenario or suite id.")
            return False
        if "avg_single_core_cpu_pct" not in summary_text or "peak_single_core_cpu_pct" not in summary_text:
            print("[FAIL] suite_summary.csv missing single-core CPU columns.")
            return False
        suite_md = open(os.path.join(summary_dir, "suite_summary.md"), "r", encoding="utf-8").read()
        if "/s" not in suite_md:
            print("[FAIL] suite_summary.md missing human-readable throughput units.")
            return False

        with open(os.path.join(scenario_dir, "longrun_summary.json"), "r", encoding="utf-8") as handle:
            longrun_data = json.load(handle)
        if longrun_data.get("longrun_status") not in ("PASS", "WARN", "FAIL", "INSUFFICIENT_DATA"):
            print("[FAIL] longrun_summary.json missing longrun_status.")
            return False
        if "single_core_cpu_start_pct" not in longrun_data:
            print("[FAIL] longrun_summary.json missing single-core CPU trend fields.")
            return False
        longrun_md = open(os.path.join(scenario_dir, "longrun_summary.md"), "r", encoding="utf-8").read()
        if "/s" not in longrun_md:
            print("[FAIL] longrun_summary.md missing human-readable throughput units.")
            return False

        print(f"[PASS] Suite mode verification succeeded under {run_dir}")
        return True

    def stage_suite_baseline_automation_test(self):
        print("\n" + "=" * 60)
        print(">>> Stage 6: Auto Baseline Generate & Compare Verification")
        print("=" * 60)

        bin_path = os.path.join(CACHE_DIR, "obs_c_bench_mock")
        auto_root = os.path.join(self.work_dir, "test_auto_baseline")
        reports_root = os.path.join(auto_root, "reports")
        logs_root = os.path.join(auto_root, "logs")
        baseline_store = os.path.join(auto_root, "baseline_store")
        baselines_manifest = os.path.join(auto_root, "baselines.csv")
        generate_suite = os.path.join(auto_root, "baseline_generate.yaml")
        compare_suite = os.path.join(auto_root, "baseline_compare.yaml")
        profile_path = os.path.join(auto_root, "baseline_profile.dat")
        perf_policy = os.path.join(self.work_dir, "ci", "perf", "gate_policy.json")
        users_path = os.path.join(self.work_dir, USERS_FILE)

        os.makedirs(auto_root, exist_ok=True)
        shutil.copy(CONFIG_FILE, profile_path)

        generate_yaml = f"""suite_id: auto_baseline_generate
defaults:
  users_file: {users_path}
profiles:
  base:
    config_file: {profile_path}
scenarios:
  - scenario_id: auto_baseline_upload_1mb_2t
    profile: base
    op: upload
    threads: 2
    object_size: 1MB
baseline:
  enabled: true
  mode: generate
  manifest: {baselines_manifest}
  store_dir: {baseline_store}
  policy: {perf_policy}
  update_strategy: new_label
reporting:
  continue_on_fail: true
"""
        compare_yaml = f"""suite_id: auto_baseline_compare
defaults:
  users_file: {users_path}
profiles:
  base:
    config_file: {profile_path}
scenarios:
  - scenario_id: auto_baseline_upload_1mb_2t
    profile: base
    op: upload
    threads: 2
    object_size: 1MB
baseline:
  enabled: true
  mode: compare
  manifest: {baselines_manifest}
  store_dir: {baseline_store}
  policy: {perf_policy}
reporting:
  continue_on_fail: true
"""
        invalid_legacy_suite = os.path.join(auto_root, "baseline_legacy_invalid.yaml")
        invalid_legacy_yaml = f"""suite_id: auto_baseline_legacy_invalid
defaults:
  users_file: {users_path}
profiles:
  base:
    config_file: {profile_path}
scenarios:
  - scenario_id: auto_baseline_upload_1mb_2t
    profile: base
    op: upload
    threads: 2
    object_size: 1MB
gates:
  perf:
    enabled: true
    mode: compare
    baselines_manifest: {baselines_manifest}
    baseline_store_dir: {baseline_store}
    policy: {perf_policy}
"""
        with open(generate_suite, "w", encoding="utf-8") as handle:
            handle.write(generate_yaml)
        with open(compare_suite, "w", encoding="utf-8") as handle:
            handle.write(compare_yaml)
        with open(invalid_legacy_suite, "w", encoding="utf-8") as handle:
            handle.write(invalid_legacy_yaml)

        ret, output = self.run_cmd(
            f"{bin_path} --suite {generate_suite} --output-dir {reports_root} --log-dir {logs_root}"
        )
        if ret != 0:
            print(output)
            print("[FAIL] Suite baseline generate run should succeed.")
            return False
        if not os.path.exists(baselines_manifest):
            print("[FAIL] Baselines manifest was not generated.")
            return False
        manifest_text = open(baselines_manifest, "r", encoding="utf-8").read()
        if "auto_baseline_upload_1mb_2t" not in manifest_text:
            print("[FAIL] Baselines manifest missing generated scenario entry.")
            return False
        if "updated_at" not in manifest_text or "baseline_source_run" not in manifest_text:
            print("[FAIL] Baselines manifest missing new metadata columns.")
            return False
        baseline_files = [name for name in os.listdir(baseline_store) if name.startswith("auto_baseline_upload_1mb_2t__")]
        if len(baseline_files) != 1:
            print("[FAIL] Expected exactly one versioned baseline artifact after generate run.")
            return False

        generate_root = os.path.join(reports_root, "auto_baseline_generate")
        generate_runs = sorted(os.listdir(generate_root))
        generate_summary_dir = os.path.join(generate_root, generate_runs[-1], "summary")
        generate_summary_text = open(os.path.join(generate_summary_dir, "suite_summary.csv"), "r", encoding="utf-8").read()
        if "BASELINE_UPDATED" not in generate_summary_text:
            print("[FAIL] Generate suite summary missing BASELINE_UPDATED status.")
            return False
        if "final_status" not in generate_summary_text or "PASS" not in generate_summary_text:
            print("[FAIL] Generate suite summary missing final_status PASS.")
            return False
        with open(os.path.join(generate_summary_dir, "suite_failures.json"), "r", encoding="utf-8") as handle:
            generate_failures = json.load(handle)
        if generate_failures:
            print("[FAIL] suite_failures.json should not treat BASELINE_UPDATED as failure.")
            return False

        ret, output = self.run_cmd(
            f"{bin_path} --suite {compare_suite} --output-dir {reports_root} --log-dir {logs_root}"
        )
        if ret != 0:
            print(output)
            print("[FAIL] Suite baseline compare run should pass against generated baseline.")
            return False

        compare_root = os.path.join(reports_root, "auto_baseline_compare")
        run_dirs = sorted(os.listdir(compare_root))
        if not run_dirs:
            print("[FAIL] Missing compare suite run directory.")
            return False
        summary_dir = os.path.join(compare_root, run_dirs[-1], "summary")
        scenario_dir = os.path.join(compare_root, run_dirs[-1], "scenarios", "auto_baseline_upload_1mb_2t")
        if not os.path.exists(os.path.join(scenario_dir, "perf_gate", "gate_result.json")):
            print("[FAIL] Missing perf_gate/gate_result.json for compare suite.")
            return False
        with open(os.path.join(scenario_dir, "perf_gate", "gate_result.json"), "r", encoding="utf-8") as handle:
            gate_result = json.load(handle)
        if gate_result.get("overall_status") != "PASS":
            print("[FAIL] Compare suite did not report PASS against generated baseline.")
            return False
        summary_text = open(os.path.join(summary_dir, "suite_summary.csv"), "r", encoding="utf-8").read()
        if "perf_status" not in summary_text or "final_status" not in summary_text:
            print("[FAIL] Compare suite summary missing perf_status/final_status columns.")
            return False
        if "PASS" not in summary_text:
            print("[FAIL] Compare suite summary missing PASS result.")
            return False
        with open(os.path.join(summary_dir, "suite_failures.json"), "r", encoding="utf-8") as handle:
            compare_failures = json.load(handle)
        if compare_failures:
            print("[FAIL] Compare suite should not produce suite_failures.json entries.")
            return False

        ret, output = self.run_cmd(
            f"{bin_path} --suite {invalid_legacy_suite} --output-dir {reports_root} --log-dir {logs_root}"
        )
        if ret == 0 or "gates.perf is no longer supported" not in output:
            print("[FAIL] Legacy gates.perf syntax should fail with a clear error.")
            return False

        print(f"[PASS] Auto baseline generate/compare verification succeeded under {auto_root}")
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
            if not self.stage_perf_gate_test():
                sys.exit(1)
            if not self.stage_suite_test():
                sys.exit(1)
            if not self.stage_suite_baseline_automation_test():
                sys.exit(1)
            self.print_summary()
        except KeyboardInterrupt:
            print("\nInterrupted.")
        finally:
            self.restore_env()

if __name__ == "__main__":
    BenchmarkTester().run()
