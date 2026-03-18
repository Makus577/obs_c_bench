#ifndef BENCH_H
#define BENCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <signal.h>
#include <limits.h>
#include "log.h" 

#ifdef MOCK_SDK_MODE
    #include "../include/mock_eSDKOBS.h"
    #define SDK_MODE_DESC "Mock SDK (Simulation Mode)"
#else
    #include "eSDKOBS.h" 
    #define SDK_MODE_DESC "Real Huawei OBS C SDK"
#endif

// 全局优雅退出标志
extern volatile sig_atomic_t g_graceful_stop;

// 定义统一的最大 Key 长度
#define MAX_KEY_LEN             1025

// TestCase 编号定义
#define TEST_CASE_PUT           201
#define TEST_CASE_GET           202
#define TEST_CASE_DELETE        204
#define TEST_CASE_MULTIPART     216
#define TEST_CASE_RESUMABLE     230
#define TEST_CASE_MIX           900

#define MAX_MIX_OPS 32 
#define MAX_RANGE_OPTIONS 64 
#define LATENCY_HIST_BUCKETS 371

// 批量落盘大小
#define BATCH_SIZE 1000

typedef struct {
    char username[64];
    char ak[128];
    char sk[128];
    char security_token[4096]; 
    char original_ak[128]; 
} UserCredential;

// 请求流水记录结构体
typedef struct {
    double timestamp_s;
    int op_type;
    char key[MAX_KEY_LEN]; 
    double latency_ms;
    int status_code;    
    int http_code;      
    long long bytes;
    char request_id[64];
} ReqRecord;

typedef struct {
    char endpoint[256];
    char protocol[16];
    int keep_alive;

    // --- 超时防卡死配置 (秒) ---
    int connect_timeout_sec;
    int request_timeout_sec;

    // --- 并发与用户配置 ---
    int threads;
    int target_user_count;
    int threads_per_user;
    char bucket_name_prefix[64];
    char bucket_name_fixed[128];

    // --- 用户列表 ---
    UserCredential *user_list;
    int loaded_user_count;

    // --- 测试计划 ---
    int requests_per_thread; 
    int test_case; 
    
    // --- 对象大小配置 ---
    long long object_size;      
    long long object_size_min;  
    long long object_size_max;  
    int is_dynamic_size;        

    // --- Range 下载配置 ---
    char *range_options[MAX_RANGE_OPTIONS];
    int range_count;

    long long part_size;
    int parts_for_each_upload_id; // [新增]: 控制多段上传的固定段数
    char key_prefix[64];
    int run_seconds;
    int allow_open_ended_run;
    
    LogLevel log_level;
    int obj_name_pattern_hash; 

    // --- 断点续传 ---
    int enable_checkpoint;      
    char upload_file_path[256]; 

    // --- 混合操作 ---
    int mix_ops[MAX_MIX_OPS];  
    int mix_op_count;          
    long long mix_loop_count;  
    int use_mix_mode;          

    // --- 安全与认证 ---
    int is_temporary_token;     
    char gm_auth_mode[32]; 
    char server_cert_path[256];
    char client_sign_cert_path[256];
    char client_sign_key_path[256];
    char client_sign_key_password[256];
    char client_enc_cert_path[256];
    char client_enc_key_path[256];

    // --- 数据校验与日志 ---
    int enable_data_validation;
    int enable_detail_log;      
    int resumable_task_num;     
    char report_task_dir[PATH_MAX];
    char log_task_dir[PATH_MAX];
    char config_file_path[PATH_MAX];
    char users_file_path[PATH_MAX];
    char object_size_spec[64];
    char scenario_id[128];
    char git_commit[64];
    char host_os[64];
    char host_arch[64];

} Config;

typedef struct {
    long long success_count;    
    long long fail_403_count;   
    long long fail_404_count;   
    long long fail_409_count;   
    long long fail_4xx_other_count; 
    long long fail_5xx_count;   
    long long fail_other_count; 
    long long fail_validation_count; 
    long long streamed_bytes;
    long long completed_success_bytes;
    double total_latency_ms;
    double max_latency_ms;
    double min_latency_ms;
    long long latency_sample_count;
    unsigned long long latency_hist[LATENCY_HIST_BUCKETS];
} ThreadStats;

typedef struct {
    int thread_id;
    Config *config;
    ThreadStats stats;
    char *data_buffer; 
    double stop_timestamp_ms;

    char effective_ak[128];
    char effective_sk[128];
    char effective_token[4096]; 
    char effective_bucket[128];
    char username[64];
    
    char *pattern_buffer;       
    long long pattern_size;     
    long long pattern_mask;
    double worker_start_time_s;
    double worker_end_time_s;
} WorkerArgs;

typedef struct {
    long long total_requests;
    long long success_requests;
    long long failed_requests;
    long long fail_403;
    long long fail_404;
    long long fail_409;
    long long fail_4xx_other;
    long long fail_5xx;
    long long fail_other;
    long long fail_validation;
    double actual_duration_s;
    double success_rate_pct;
    double final_tps;
    double peak_tps;
    double final_bps;
    double peak_bps;
    double avg_latency_ms;
    double p99_latency_ms;
    double avg_single_stream_bps;
    double max_single_stream_bps;
    double avg_cpu_pct;
    double peak_cpu_pct;
    double avg_single_core_cpu_pct;
    double peak_single_core_cpu_pct;
    double avg_rss_mb;
    double peak_rss_mb;
} BenchmarkSummary;

static inline int latency_histogram_bucket(double latency_ms) {
    if (latency_ms <= 0.0) return 0;
    if (latency_ms < 10.0) return (int)(latency_ms * 10.0);
    if (latency_ms < 100.0) return 100 + (int)(latency_ms - 10.0);
    if (latency_ms < 1000.0) return 190 + (int)((latency_ms - 100.0) / 10.0);
    if (latency_ms < 10000.0) return 280 + (int)((latency_ms - 1000.0) / 100.0);
    return LATENCY_HIST_BUCKETS - 1;
}

static inline double latency_bucket_upper_bound_ms(int bucket) {
    if (bucket < 0) return 0.0;
    if (bucket < 100) return (bucket + 1) / 10.0;
    if (bucket < 190) return 10.0 + (bucket - 100 + 1);
    if (bucket < 280) return 100.0 + (bucket - 190 + 1) * 10.0;
    if (bucket < LATENCY_HIST_BUCKETS - 1) return 1000.0 + (bucket - 280 + 1) * 100.0;
    return 10000.0;
}

// 函数声明
int load_config(const char *filename, Config *cfg);
int load_users_file(const char *filename, Config *cfg, int is_temp_mode); 
int parse_object_size_spec(const char *spec, Config *cfg, char *errbuf, size_t errbuf_size);
int parse_test_case_arg(const char *value, int *out_test_case);
const char *test_case_to_name(int test_case);
void *worker_routine(void *arg);
void fill_pattern_buffer(char *buf, size_t size, int seed);

void save_benchmark_report(Config *cfg, const BenchmarkSummary *summary);

obs_status run_put_benchmark(WorkerArgs *args, char *key, long long object_size, char *out_req_id);
obs_status run_get_benchmark(WorkerArgs *args, char *key, char *range_str, char *out_req_id);
obs_status run_delete_benchmark(WorkerArgs *args, char *key, char *out_req_id);
obs_status run_list_benchmark(WorkerArgs *args, char *out_req_id);
obs_status run_multipart_benchmark(WorkerArgs *args, char *key, char *out_req_id);
obs_status run_upload_file_benchmark(WorkerArgs *args, char *key, char *out_req_id);

#endif
