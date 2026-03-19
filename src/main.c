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

volatile sig_atomic_t g_graceful_stop = 0;

typedef struct {
    WorkerArgs *t_args;
    int thread_count;
    int interval_sec;
    volatile int stop_flag;
    char log_task_dir[PATH_MAX];
    int sample_count;
    int cpu_sample_count;
    int single_core_cpu_sample_count;
    int rss_sample_count;
    int wrote_samples;
    long long prev_total_requests;
    long long prev_streamed_bytes;
    double start_wall_s;
    double prev_wall_s;
    double prev_cpu_s;
    double cpu_sum_pct;
    double cpu_peak_pct;
    double single_core_cpu_sum_pct;
    double single_core_cpu_peak_pct;
    double rss_sum_mb;
    double rss_peak_mb;
    double peak_tps;
    double peak_bps;
    int logical_cpu_count;
} MonitorArgs;

typedef struct {
    char config_file[PATH_MAX];
    char users_file[PATH_MAX];
    char output_dir[PATH_MAX];
    char log_dir[PATH_MAX];
    char suite_file[PATH_MAX];
    char scenario_id[128];
    char object_size_spec[64];
    int op_set;
    int op;
    int threads_set;
    int threads;
    int object_size_set;
    int output_dir_set;
    int log_dir_set;
    int suite_mode;
} CliOptions;

typedef struct {
    char suite_id[128];
    char scenario_id[128];
    char profile[128];
    char config_file[PATH_MAX];
    char users_file[PATH_MAX];
    char op[32];
    char object_size_spec[64];
    char longrun_policy[PATH_MAX];
    char baseline_mode[32];
    char baseline_policy[PATH_MAX];
    char baseline_manifest[PATH_MAX];
    char baseline_store_dir[PATH_MAX];
    char baseline_update_strategy[32];
    int threads;
    int run_seconds_set;
    int run_seconds;
    int requests_per_thread_set;
    int requests_per_thread;
    int allow_open_ended_run;
    int continue_on_fail;
    int analyze_longrun;
    int gate_longrun;
    int baseline_enabled;
} SuiteScenario;

typedef struct {
    int exit_status;
    int longrun_exit_status;
    int baseline_exit_status;
    int effective_run_seconds;
    int effective_requests_per_thread;
    int analyze_longrun;
    int gate_longrun;
    char report_dir[PATH_MAX];
    char log_dir[PATH_MAX];
} ScenarioRunResult;

void handle_sigint(int sig) {
    static volatile sig_atomic_t sigint_count = 0;
    (void)sig;
    sigint_count++;

    if (sigint_count == 1) {
        g_graceful_stop = 1;
        {
            const char msg[] = "\n[WARN] Received SIGINT (1/2). Initiating graceful shutdown... Please wait.\n"
                               "       Press CTRL+C again to FORCE QUIT if it hangs.\n";
            if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0) { }
        }
    } else {
        const char msg[] = "\n[FATAL] Received SIGINT (2/2). Force quitting immediately!\n";
        if (write(STDOUT_FILENO, msg, sizeof(msg) - 1) < 0) { }
        _exit(1);
    }
}

const char* log_level_to_string(LogLevel level) {
    switch(level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_OFF:   return "OFF";
        default:        return "UNKNOWN";
    }
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static double process_cpu_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        return -1.0;
    }
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static double read_process_rss_mb(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    char line[256];
    long long rss_kb = -1;

    if (!fp) return -1.0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%lld", &rss_kb);
            break;
        }
    }
    fclose(fp);
    if (rss_kb < 0) return -1.0;
    return rss_kb / 1024.0;
}

static int get_logical_cpu_count(void) {
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count <= 0) return 1;
    return (int)cpu_count;
}

static void format_duration_human(double seconds, char *buf, size_t buf_size) {
    long long total_seconds;
    long long hours;
    long long minutes;
    long long secs;

    if (!buf || buf_size == 0) return;
    if (seconds < 0.0) {
        snprintf(buf, buf_size, "N/A");
        return;
    }
    total_seconds = (long long)(seconds + 0.5);
    hours = total_seconds / 3600;
    minutes = (total_seconds % 3600) / 60;
    secs = total_seconds % 60;
    if (hours > 0) snprintf(buf, buf_size, "%lldh %lldm %llds", hours, minutes, secs);
    else if (minutes > 0) snprintf(buf, buf_size, "%lldm %llds", minutes, secs);
    else snprintf(buf, buf_size, "%.2fs", seconds);
}

static void format_bytes_si(long long bytes, char *buf, size_t buf_size) {
    const char *units[] = {"Bytes", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit_idx = 0;

    if (!buf || buf_size == 0) return;
    if (bytes < 0) {
        snprintf(buf, buf_size, "N/A");
        return;
    }
    while (value >= 1000.0 && unit_idx < 4) {
        value /= 1000.0;
        unit_idx++;
    }
    if (unit_idx == 0) snprintf(buf, buf_size, "%lld %s", bytes, units[unit_idx]);
    else snprintf(buf, buf_size, "%.2f %s", value, units[unit_idx]);
}

static void format_rate_si(double bytes_per_sec, char *buf, size_t buf_size) {
    const char *units[] = {"Bytes/s", "KB/s", "MB/s", "GB/s", "TB/s"};
    double value = bytes_per_sec;
    int unit_idx = 0;

    if (!buf || buf_size == 0) return;
    if (bytes_per_sec < 0.0) {
        snprintf(buf, buf_size, "N/A");
        return;
    }
    while (value >= 1000.0 && unit_idx < 4) {
        value /= 1000.0;
        unit_idx++;
    }
    snprintf(buf, buf_size, "%.2f %s", value, units[unit_idx]);
}

static void format_memory_mb_human(double value_mb, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (value_mb < 0.0) {
        snprintf(buf, buf_size, "N/A");
        return;
    }
    if (value_mb >= 1000.0) snprintf(buf, buf_size, "%.2f GB", value_mb / 1000.0);
    else snprintf(buf, buf_size, "%.2f MB", value_mb);
}

static void format_latency_human(double latency_ms, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (latency_ms < 0.0) {
        snprintf(buf, buf_size, "N/A");
        return;
    }
    if (latency_ms >= 1000.0) snprintf(buf, buf_size, "%.2f s", latency_ms / 1000.0);
    else snprintf(buf, buf_size, "%.2f ms", latency_ms);
}

static int ensure_directory(const char *path) {
    char tmp[PATH_MAX];
    char *p;
    struct stat st;

    if (!path || path[0] == '\0') return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0755) != 0) return -1;
            } else if (!S_ISDIR(st.st_mode)) {
                return -1;
            }
            *p = '/';
        }
    }
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, 0755) != 0) return -1;
    } else if (!S_ISDIR(st.st_mode)) {
        return -1;
    }
    return 0;
}

static void detect_host_metadata(Config *cfg) {
    struct utsname uts;

    if (uname(&uts) == 0) {
        snprintf(cfg->host_os, sizeof(cfg->host_os), "%s", uts.sysname);
        snprintf(cfg->host_arch, sizeof(cfg->host_arch), "%s", uts.machine);
    } else {
        snprintf(cfg->host_os, sizeof(cfg->host_os), "%s", "unknown");
        snprintf(cfg->host_arch, sizeof(cfg->host_arch), "%s", "unknown");
    }
}

static void load_git_commit_metadata(Config *cfg) {
    const char *git_commit = getenv("OBS_BENCH_GIT_COMMIT");

    if (git_commit && git_commit[0] != '\0') {
        snprintf(cfg->git_commit, sizeof(cfg->git_commit), "%s", git_commit);
    } else {
        snprintf(cfg->git_commit, sizeof(cfg->git_commit), "%s", "unknown");
    }
}

static void infer_scenario_id(const Config *cfg, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "%s_%s_%dt",
             test_case_to_name(cfg->test_case),
             cfg->object_size_spec[0] ? cfg->object_size_spec : "default",
             cfg->threads > 0 ? cfg->threads : 0);
}

static const char *execution_mode_label(const Config *cfg) {
    if (!cfg) return "Unknown";
    if (cfg->run_seconds > 0) return "Time-Limited";
    if (cfg->test_case == TEST_CASE_MIX) {
        if (cfg->mix_loop_count > 0 && cfg->requests_per_thread > 0) return "Request-Limited";
    } else if (cfg->requests_per_thread > 0) {
        return "Request-Limited";
    }
    if (cfg->allow_open_ended_run) return "Open-Ended";
    return "Invalid";
}

static const char *config_source_label(int source) {
    switch (source) {
        case CONFIG_SOURCE_CONFIG: return "config.dat";
        case CONFIG_SOURCE_CLI: return "CLI";
        case CONFIG_SOURCE_SUITE: return "suite";
        case CONFIG_SOURCE_DEFAULT:
        default:
            return "default";
    }
}

static const char *bool_to_word(int value) {
    return value ? "true" : "false";
}

static int mix_contains_op(const Config *cfg, int test_case) {
    int i;

    if (!cfg || cfg->test_case != TEST_CASE_MIX) return 0;
    for (i = 0; i < cfg->mix_op_count; i++) {
        if (cfg->mix_ops[i] == test_case) return 1;
    }
    return 0;
}

static void sanitize_markdown_cell(const char *input, char *output, size_t output_size) {
    size_t i = 0;
    size_t j = 0;

    if (!output || output_size == 0) return;
    output[0] = '\0';
    if (!input) return;

    while (input[i] != '\0' && j + 1 < output_size) {
        if (input[i] == '|') {
            if (j + 2 >= output_size) break;
            output[j++] = '\\';
            output[j++] = '|';
        } else if (input[i] == '\n' || input[i] == '\r' || input[i] == '\t') {
            output[j++] = ' ';
        } else {
            output[j++] = input[i];
        }
        i++;
    }
    output[j] = '\0';
}

static void write_effective_config_row(FILE *fp,
                                       const char *field,
                                       const char *value,
                                       int source,
                                       int active,
                                       const char *impact) {
    char field_cell[128];
    char value_cell[512];
    char impact_cell[512];

    if (!fp) return;
    sanitize_markdown_cell(field, field_cell, sizeof(field_cell));
    sanitize_markdown_cell(value, value_cell, sizeof(value_cell));
    sanitize_markdown_cell(impact, impact_cell, sizeof(impact_cell));
    fprintf(fp, "| `%s` | `%s` | `%s` | `%s` | %s |\n",
            field_cell,
            value_cell[0] ? value_cell : "N/A",
            config_source_label(source),
            active ? "yes" : "no",
            impact_cell[0] ? impact_cell : "N/A");
}

static int is_field_active_for_case(const Config *cfg, const char *field_name) {
    if (!cfg || !field_name) return 0;
    if (strcmp(field_name, "RunSeconds") == 0) return 1;
    if (strcmp(field_name, "RequestsPerThread") == 0) return 1;
    if (strcmp(field_name, "AllowOpenEndedRun") == 0) return 1;
    if (strcmp(field_name, "Range") == 0) {
        return cfg->test_case == TEST_CASE_GET || mix_contains_op(cfg, TEST_CASE_GET);
    }
    if (strcmp(field_name, "PartSize") == 0) {
        return cfg->test_case == TEST_CASE_MULTIPART ||
               cfg->test_case == TEST_CASE_RESUMABLE ||
               mix_contains_op(cfg, TEST_CASE_MULTIPART) ||
               mix_contains_op(cfg, TEST_CASE_RESUMABLE);
    }
    if (strcmp(field_name, "PartsForEachUploadID") == 0) {
        return cfg->test_case == TEST_CASE_MULTIPART || mix_contains_op(cfg, TEST_CASE_MULTIPART);
    }
    if (strcmp(field_name, "UploadFilePath") == 0 || strcmp(field_name, "EnableCheckpoint") == 0) {
        return cfg->test_case == TEST_CASE_RESUMABLE || mix_contains_op(cfg, TEST_CASE_RESUMABLE);
    }
    if (strcmp(field_name, "ObjectSize") == 0) {
        if (cfg->test_case == TEST_CASE_DELETE) return 0;
        if (cfg->test_case == TEST_CASE_MIX) {
            return mix_contains_op(cfg, TEST_CASE_PUT) ||
                   mix_contains_op(cfg, TEST_CASE_GET) ||
                   mix_contains_op(cfg, TEST_CASE_MULTIPART) ||
                   mix_contains_op(cfg, TEST_CASE_RESUMABLE);
        }
        return 1;
    }
    return 1;
}

static void warn_unused_config_if_set(const char *field,
                                      int source,
                                      int should_warn,
                                      const char *value,
                                      const char *reason) {
    if (!should_warn || source == CONFIG_SOURCE_DEFAULT) return;
    printf("[Config Warning] %s=%s is ignored for this %s scenario. %s\n",
           field,
           value && value[0] ? value : "N/A",
           reason ? reason : "operation",
           "See effective_config.md for field relevance details.");
}

static void emit_irrelevant_config_warnings(const Config *cfg) {
    char part_size_value[128];
    char parts_value[64];
    char range_value[256];

    if (!cfg) return;
    snprintf(part_size_value, sizeof(part_size_value), "%lld", cfg->part_size);
    snprintf(parts_value, sizeof(parts_value), "%d", cfg->parts_for_each_upload_id);
    if (cfg->range_count > 0) {
        snprintf(range_value, sizeof(range_value), "%s", cfg->range_options[0]);
        if (cfg->range_count > 1) {
            snprintf(range_value + strlen(range_value),
                     sizeof(range_value) - strlen(range_value),
                     " ... (%d total)",
                     cfg->range_count);
        }
    } else {
        snprintf(range_value, sizeof(range_value), "%s", "empty");
    }

    if (!is_field_active_for_case(cfg, "Range") && cfg->range_count > 0) {
        warn_unused_config_if_set("Range", cfg->range_source, 1, range_value, test_case_to_name(cfg->test_case));
    }
    if (!is_field_active_for_case(cfg, "PartSize")) {
        warn_unused_config_if_set("PartSize", cfg->part_size_source, 1, part_size_value, test_case_to_name(cfg->test_case));
    }
    if (!is_field_active_for_case(cfg, "PartsForEachUploadID") && cfg->parts_for_each_upload_id > 0) {
        warn_unused_config_if_set("PartsForEachUploadID", cfg->parts_for_each_upload_id_source, 1, parts_value, test_case_to_name(cfg->test_case));
    }
    if (!is_field_active_for_case(cfg, "UploadFilePath") && cfg->upload_file_path[0] != '\0') {
        warn_unused_config_if_set("UploadFilePath", cfg->upload_file_path_source, 1, cfg->upload_file_path, test_case_to_name(cfg->test_case));
    }
    if (!is_field_active_for_case(cfg, "EnableCheckpoint")) {
        warn_unused_config_if_set("EnableCheckpoint",
                                  cfg->enable_checkpoint_source,
                                  cfg->enable_checkpoint_source != CONFIG_SOURCE_DEFAULT,
                                  bool_to_word(cfg->enable_checkpoint),
                                  test_case_to_name(cfg->test_case));
    }
    if (!is_field_active_for_case(cfg, "ObjectSize") && cfg->object_size_source != CONFIG_SOURCE_DEFAULT) {
        warn_unused_config_if_set("ObjectSize", cfg->object_size_source, 1, cfg->object_size_spec, test_case_to_name(cfg->test_case));
    }
}

static int write_effective_config_report(const Config *cfg) {
    FILE *fp;
    char range_value[512];
    char part_size_value[128];
    char object_size_value[128];
    char threads_value[64];
    char run_seconds_value[64];
    char requests_value[64];
    char parts_value[64];
    int i;

    if (!cfg || cfg->effective_config_path[0] == '\0') return -1;
    fp = fopen(cfg->effective_config_path, "w");
    if (!fp) return -1;

    if (cfg->range_count > 0) {
        range_value[0] = '\0';
        for (i = 0; i < cfg->range_count; i++) {
            if (i > 0) strncat(range_value, "; ", sizeof(range_value) - strlen(range_value) - 1);
            strncat(range_value, cfg->range_options[i], sizeof(range_value) - strlen(range_value) - 1);
        }
    } else {
        snprintf(range_value, sizeof(range_value), "%s", "N/A");
    }

    snprintf(part_size_value, sizeof(part_size_value), "%lld Bytes", cfg->part_size);
    if (cfg->is_dynamic_size) {
        snprintf(object_size_value,
                 sizeof(object_size_value),
                 "%s (%lld~%lld Bytes)",
                 cfg->object_size_spec,
                 cfg->object_size_min,
                 cfg->object_size_max);
    } else {
        snprintf(object_size_value,
                 sizeof(object_size_value),
                 "%s (%lld Bytes)",
                 cfg->object_size_spec,
                 cfg->object_size_max);
    }
    snprintf(threads_value, sizeof(threads_value), "%d", cfg->threads);
    snprintf(run_seconds_value, sizeof(run_seconds_value), "%d", cfg->run_seconds);
    snprintf(requests_value, sizeof(requests_value), "%d", cfg->requests_per_thread);
    snprintf(parts_value, sizeof(parts_value), "%d", cfg->parts_for_each_upload_id);

    fprintf(fp, "# Effective Config\n\n");
    fprintf(fp, "- Scenario ID: `%s`\n", cfg->scenario_id[0] ? cfg->scenario_id : "N/A");
    fprintf(fp, "- Operation: `%s`\n", test_case_to_name(cfg->test_case));
    fprintf(fp, "- Execution Mode: `%s`\n", execution_mode_label(cfg));
    fprintf(fp, "- Report Directory: `%s`\n", cfg->report_task_dir);
    fprintf(fp, "- Log Directory: `%s`\n", cfg->log_task_dir);
    fprintf(fp, "- Config File: `%s`\n", cfg->config_file_path[0] ? cfg->config_file_path : "N/A");
    fprintf(fp, "- Users File: `%s`\n", cfg->users_file_path[0] ? cfg->users_file_path : "N/A");
    fprintf(fp, "\n");
    if (cfg->unknown_config_key_count > 0) {
        fprintf(fp, "## Unknown keys from config.dat\n\n");
        for (i = 0; i < cfg->unknown_config_key_count; i++) {
            fprintf(fp, "- `%s`\n", cfg->unknown_config_keys[i]);
        }
        fprintf(fp, "\n");
    }
    fprintf(fp, "## Effective Fields\n\n");
    fprintf(fp, "| Field | Final Value | Source | Active In Current Scenario | Impact |\n");
    fprintf(fp, "| --- | --- | --- | --- | --- |\n");
    write_effective_config_row(fp, "TestCase", test_case_to_name(cfg->test_case), CONFIG_SOURCE_DEFAULT,
                               1, "Selects the benchmark operation family and determines which other parameters are meaningful.");
    write_effective_config_row(fp, "RunSeconds", run_seconds_value, cfg->run_seconds_source,
                               is_field_active_for_case(cfg, "RunSeconds"),
                               "Controls time-limited stop conditions. When >0, the run stops by elapsed time.");
    write_effective_config_row(fp, "RequestsPerThread", requests_value, cfg->requests_per_thread_source,
                               is_field_active_for_case(cfg, "RequestsPerThread"),
                               "Controls per-thread request budget when the scenario is request-limited.");
    write_effective_config_row(fp, "AllowOpenEndedRun", bool_to_word(cfg->allow_open_ended_run), cfg->allow_open_ended_run_source,
                               is_field_active_for_case(cfg, "AllowOpenEndedRun"),
                               "Allows runs with no automatic stop condition. Use only when you intend to stop the run manually.");
    write_effective_config_row(fp, "Threads", threads_value, cfg->threads_source,
                               1,
                               "Controls total concurrency. Multi-user runs distribute these threads across loaded users.");
    write_effective_config_row(fp, "ObjectSize", object_size_value, cfg->object_size_source,
                               is_field_active_for_case(cfg, "ObjectSize"),
                               "Controls payload size for upload/download-style traffic. Delete only uses object keys, not payload bytes.");
    write_effective_config_row(fp, "Range", range_value, cfg->range_source,
                               is_field_active_for_case(cfg, "Range"),
                               "Only affects download/get requests. Each configured range entry becomes a selectable byte-range request.");
    write_effective_config_row(fp, "PartSize", part_size_value, cfg->part_size_source,
                               is_field_active_for_case(cfg, "PartSize"),
                               "Controls multipart/resumable chunk granularity and directly affects part count and transfer behavior.");
    write_effective_config_row(fp, "PartsForEachUploadID", parts_value, cfg->parts_for_each_upload_id_source,
                               is_field_active_for_case(cfg, "PartsForEachUploadID"),
                               "Only affects multipart upload. Combined with PartSize to determine total multipart object size.");
    write_effective_config_row(fp, "UploadFilePath", cfg->upload_file_path[0] ? cfg->upload_file_path : "N/A", cfg->upload_file_path_source,
                               is_field_active_for_case(cfg, "UploadFilePath"),
                               "Only affects resumable upload. Points to the local file used by upload-file/checkpoint flow.");
    write_effective_config_row(fp, "EnableCheckpoint", bool_to_word(cfg->enable_checkpoint), cfg->enable_checkpoint_source,
                               is_field_active_for_case(cfg, "EnableCheckpoint"),
                               "Only affects resumable upload. Enables checkpoint persistence for resume/retry behavior.");
    write_effective_config_row(fp, "EnableDetailLog", bool_to_word(cfg->enable_detail_log), cfg->enable_detail_log_source,
                               1,
                               "Controls whether per-request detail_*.csv logs are written.");
    write_effective_config_row(fp, "GmAuthMode", cfg->gm_auth_mode[0] ? cfg->gm_auth_mode : "Default", cfg->gm_auth_mode_source,
                               1,
                               "Controls GM / interworking TLS authentication mode and certificate requirements.");
    write_effective_config_row(fp, "ServerCertPath", cfg->server_cert_path[0] ? cfg->server_cert_path : "N/A", cfg->server_cert_path_source,
                               1,
                               "Used when one-way or mutual TLS validation requires a server-side trust certificate.");
    write_effective_config_row(fp, "ClientSignCertPath", cfg->client_sign_cert_path[0] ? cfg->client_sign_cert_path : "N/A", cfg->client_sign_cert_path_source,
                               1,
                               "Used by mutual-auth scenarios for client signing identity.");
    write_effective_config_row(fp, "ClientSignKeyPath", cfg->client_sign_key_path[0] ? cfg->client_sign_key_path : "N/A", cfg->client_sign_key_path_source,
                               1,
                               "Used by mutual-auth scenarios for the client signing private key.");
    write_effective_config_row(fp, "ClientSignKeyPassword", cfg->client_sign_key_password[0] ? "(set)" : "N/A", cfg->client_sign_key_password_source,
                               1,
                               "Password used to unlock the client signing key when required.");
    write_effective_config_row(fp, "ClientEncCertPath", cfg->client_enc_cert_path[0] ? cfg->client_enc_cert_path : "N/A", cfg->client_enc_cert_path_source,
                               1,
                               "Used by GM mutual-auth scenarios for client encryption certificate material.");
    write_effective_config_row(fp, "ClientEncKeyPath", cfg->client_enc_key_path[0] ? cfg->client_enc_key_path : "N/A", cfg->client_enc_key_path_source,
                               1,
                               "Used by GM mutual-auth scenarios for client encryption private key material.");

    fclose(fp);
    return 0;
}

static int is_supported_test_case(int test_case) {
    switch (test_case) {
        case TEST_CASE_PUT:
        case TEST_CASE_GET:
        case TEST_CASE_DELETE:
        case TEST_CASE_MULTIPART:
        case TEST_CASE_RESUMABLE:
        case TEST_CASE_MIX:
            return 1;
        default:
            return 0;
    }
}

static int validate_runtime_config(const Config *cfg, int using_explicit_total_threads, char *errbuf, size_t errbuf_size) {
    long long multipart_total_bytes = 0;
    int request_limited = 0;

    if (!cfg || !errbuf || errbuf_size == 0) return -1;
    errbuf[0] = '\0';

    if (!is_supported_test_case(cfg->test_case)) {
        snprintf(errbuf, errbuf_size, "Unsupported TestCase: %d", cfg->test_case);
        return -1;
    }
    if (cfg->run_seconds < 0) {
        snprintf(errbuf, errbuf_size, "RunSeconds must be >= 0. Current value: %d", cfg->run_seconds);
        return -1;
    }
    if (cfg->requests_per_thread < 0) {
        snprintf(errbuf, errbuf_size, "RequestsPerThread must be >= 0. Current value: %d", cfg->requests_per_thread);
        return -1;
    }
    if (cfg->loaded_user_count <= 0) {
        snprintf(errbuf, errbuf_size, "No users loaded from %s", cfg->users_file_path[0] ? cfg->users_file_path : "users file");
        return -1;
    }
    if (!using_explicit_total_threads && cfg->threads_per_user <= 0) {
        snprintf(errbuf, errbuf_size, "ThreadsPerUser must be > 0 when total threads are derived from config. Current value: %d", cfg->threads_per_user);
        return -1;
    }
    if (cfg->threads <= 0) {
        snprintf(errbuf, errbuf_size, "Total threads must be > 0. Current value: %d", cfg->threads);
        return -1;
    }
    if (cfg->object_size_min <= 0 || cfg->object_size_max <= 0) {
        snprintf(errbuf, errbuf_size, "ObjectSize must be > 0. Current range: %lld ~ %lld Bytes", cfg->object_size_min, cfg->object_size_max);
        return -1;
    }
    if (cfg->object_size_min > cfg->object_size_max) {
        snprintf(errbuf, errbuf_size, "ObjectSize range is invalid. Current range: %lld ~ %lld Bytes", cfg->object_size_min, cfg->object_size_max);
        return -1;
    }
    if (cfg->part_size <= 0) {
        snprintf(errbuf, errbuf_size, "PartSize must be > 0. Current value: %lld", cfg->part_size);
        return -1;
    }
    if (cfg->test_case == TEST_CASE_MULTIPART) {
        if (cfg->parts_for_each_upload_id <= 0) {
            snprintf(errbuf, errbuf_size, "Multipart upload requires PartsForEachUploadID > 0. Current value: %d", cfg->parts_for_each_upload_id);
            return -1;
        }
        if (cfg->part_size > 0 && cfg->parts_for_each_upload_id > 0 &&
            cfg->part_size > LLONG_MAX / cfg->parts_for_each_upload_id) {
            snprintf(errbuf, errbuf_size, "Multipart total size overflow risk: PartSize=%lld, PartsForEachUploadID=%d",
                     cfg->part_size, cfg->parts_for_each_upload_id);
            return -1;
        }
        multipart_total_bytes = cfg->part_size * (long long)cfg->parts_for_each_upload_id;
        if (multipart_total_bytes <= 0) {
            snprintf(errbuf, errbuf_size, "Multipart total object size must be > 0. Current value: %lld", multipart_total_bytes);
            return -1;
        }
    }
    if (cfg->test_case == TEST_CASE_RESUMABLE && cfg->upload_file_path[0] == '\0') {
        snprintf(errbuf, errbuf_size, "Resumable upload requires UploadFilePath to be configured.");
        return -1;
    }
    if ((cfg->analyze_longrun || cfg->gate_longrun) && cfg->run_seconds <= 0) {
        snprintf(
            errbuf,
            errbuf_size,
            "Long-run scenario '%s' requires RunSeconds > 0. Effective RunSeconds=%d, AnalyzeLongrun=%s, GateLongrun=%s",
            cfg->scenario_id[0] ? cfg->scenario_id : "unknown",
            cfg->run_seconds,
            cfg->analyze_longrun ? "true" : "false",
            cfg->gate_longrun ? "true" : "false"
        );
        return -1;
    }
    if (cfg->test_case == TEST_CASE_MIX) {
        if (cfg->mix_op_count <= 0) {
            snprintf(errbuf, errbuf_size, "Mix mode requires MixOperation to contain at least one valid operation.");
            return -1;
        }
        if (cfg->mix_loop_count < 0) {
            snprintf(errbuf, errbuf_size, "MixLoopCount must be >= 0. Current value: %lld", cfg->mix_loop_count);
            return -1;
        }
        request_limited = (cfg->mix_loop_count > 0 && cfg->requests_per_thread > 0);
    } else {
        request_limited = (cfg->requests_per_thread > 0);
    }
    if (cfg->run_seconds <= 0 && !request_limited && !cfg->allow_open_ended_run) {
        snprintf(errbuf, errbuf_size,
                 "Open-ended run is blocked by default. Set RunSeconds > 0 or provide a bounded request plan, or explicitly enable AllowOpenEndedRun=true.");
        return -1;
    }

    return 0;
}

static void str_tolower(char *dst, const char *src) {
    while(*src) {
        *dst = (char)tolower((unsigned char)*src);
        dst++;
        src++;
    }
    *dst = '\0';
}

static double compute_progress_pct(Config *cfg, long long current_total) {
    double progress_pct = -1.0;

    if (cfg->run_seconds > 0) {
        progress_pct = (current_total >= 0) ? -2.0 : -1.0;
    } else {
        long long reqs_per_op = cfg->requests_per_thread > 0 ? cfg->requests_per_thread : 1;
        long long expected_total_reqs = 0;

        if (cfg->use_mix_mode) {
            expected_total_reqs = (long long)cfg->threads * cfg->mix_op_count * cfg->mix_loop_count * reqs_per_op;
        } else if (cfg->requests_per_thread > 0) {
            expected_total_reqs = (long long)cfg->threads * reqs_per_op;
        }

        if (expected_total_reqs > 0) {
            progress_pct = ((double)current_total / expected_total_reqs) * 100.0;
        }
    }

    if (progress_pct > 100.0) progress_pct = 100.0;
    return progress_pct;
}

static void gather_thread_counters(const MonitorArgs *m_args,
                                   long long *success,
                                   long long *fail,
                                   long long *streamed_bytes) {
    int i;
    *success = 0;
    *fail = 0;
    *streamed_bytes = 0;

    for (i = 0; i < m_args->thread_count; i++) {
        *success += m_args->t_args[i].stats.success_count;
        *streamed_bytes += m_args->t_args[i].stats.streamed_bytes;
        *fail += (m_args->t_args[i].stats.fail_403_count +
                  m_args->t_args[i].stats.fail_404_count +
                  m_args->t_args[i].stats.fail_409_count +
                  m_args->t_args[i].stats.fail_4xx_other_count +
                  m_args->t_args[i].stats.fail_5xx_count +
                  m_args->t_args[i].stats.fail_other_count +
                  m_args->t_args[i].stats.fail_validation_count);
    }
}

static void emit_monitor_snapshot(MonitorArgs *m_args, FILE *rt_fp, int force_snapshot) {
    long long current_success = 0;
    long long current_fail = 0;
    long long current_streamed_bytes = 0;
    long long current_total = 0;
    double now_wall_s = monotonic_seconds();
    double elapsed_s = now_wall_s - m_args->start_wall_s;
    double delta_wall_s = now_wall_s - m_args->prev_wall_s;
    double current_cpu_s = process_cpu_seconds();
    double cpu_pct = -1.0;
    double single_core_cpu_pct = -1.0;
    double rss_mb = read_process_rss_mb();
    double interval_tps = 0.0;
    double interval_bps = 0.0;
    double cumul_tps = 0.0;
    double cumul_bps = 0.0;
    double success_rate = 0.0;
    double progress_pct;
    Config *cfg;

    gather_thread_counters(m_args, &current_success, &current_fail, &current_streamed_bytes);
    current_total = current_success + current_fail;

    if (elapsed_s < 0.0) elapsed_s = 0.0;
    if (delta_wall_s <= 0.0) delta_wall_s = force_snapshot ? 0.000001 : 0.0;
    if (!force_snapshot && delta_wall_s <= 0.0) return;

    if (current_cpu_s >= 0.0 && m_args->prev_cpu_s >= 0.0 && delta_wall_s > 0.0) {
        cpu_pct = ((current_cpu_s - m_args->prev_cpu_s) / delta_wall_s) * 100.0;
        if (cpu_pct < 0.0) cpu_pct = 0.0;
        single_core_cpu_pct = cpu_pct / (m_args->logical_cpu_count > 0 ? m_args->logical_cpu_count : 1);
    }

    if (delta_wall_s > 0.0) {
        interval_tps = (current_total - m_args->prev_total_requests) / delta_wall_s;
        interval_bps = (current_streamed_bytes - m_args->prev_streamed_bytes) / delta_wall_s;
    }
    if (elapsed_s > 0.0) {
        cumul_tps = current_total / elapsed_s;
        cumul_bps = current_streamed_bytes / elapsed_s;
    }
    if (current_total > 0) {
        success_rate = ((double)current_success / current_total) * 100.0;
    }

    if (interval_tps > m_args->peak_tps) m_args->peak_tps = interval_tps;
    if (interval_bps > m_args->peak_bps) m_args->peak_bps = interval_bps;
    if (cpu_pct >= 0.0) {
        m_args->cpu_sum_pct += cpu_pct;
        m_args->cpu_sample_count++;
        if (cpu_pct > m_args->cpu_peak_pct) m_args->cpu_peak_pct = cpu_pct;
    }
    if (single_core_cpu_pct >= 0.0) {
        m_args->single_core_cpu_sum_pct += single_core_cpu_pct;
        m_args->single_core_cpu_sample_count++;
        if (single_core_cpu_pct > m_args->single_core_cpu_peak_pct) m_args->single_core_cpu_peak_pct = single_core_cpu_pct;
    }
    if (rss_mb >= 0.0) {
        m_args->rss_sum_mb += rss_mb;
        m_args->rss_sample_count++;
        if (rss_mb > m_args->rss_peak_mb) m_args->rss_peak_mb = rss_mb;
    }
    m_args->sample_count++;
    m_args->wrote_samples = 1;

    cfg = m_args->thread_count > 0 ? m_args->t_args[0].config : NULL;
    progress_pct = cfg ? compute_progress_pct(cfg, current_total) : -1.0;
    if (cfg && cfg->run_seconds > 0 && elapsed_s > 0.0) {
        progress_pct = (elapsed_s / cfg->run_seconds) * 100.0;
        if (progress_pct > 100.0) progress_pct = 100.0;
    }

    if (progress_pct >= 0.0) {
        printf("[Monitor] RunTime: %8.1fs | Process: %6.2f%% | CPU: %7.2f%% | RSS: %8.2f MB | Interval TPS: %8.2f | Interval BPS: %12.2f | Cumul TPS: %8.2f | Cumul BPS: %12.2f | Success Rate: %7.3f%% | Total Reqs: %lld\n",
               elapsed_s, progress_pct, cpu_pct, rss_mb, interval_tps, interval_bps, cumul_tps, cumul_bps, success_rate, current_total);
    } else {
        printf("[Monitor] RunTime: %8.1fs | Process:    N/A | CPU: %7.2f%% | RSS: %8.2f MB | Interval TPS: %8.2f | Interval BPS: %12.2f | Cumul TPS: %8.2f | Cumul BPS: %12.2f | Success Rate: %7.3f%% | Total Reqs: %lld\n",
               elapsed_s, cpu_pct, rss_mb, interval_tps, interval_bps, cumul_tps, cumul_bps, success_rate, current_total);
    }

    if (rt_fp) {
        fprintf(rt_fp, "%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%lld\n",
                elapsed_s,
                progress_pct >= 0.0 ? progress_pct : 0.0,
                cpu_pct,
                single_core_cpu_pct,
                rss_mb,
                interval_tps,
                interval_bps,
                cumul_tps,
                cumul_bps,
                m_args->peak_tps,
                m_args->peak_bps,
                success_rate,
                current_total);
        fflush(rt_fp);
    }

    m_args->prev_total_requests = current_total;
    m_args->prev_streamed_bytes = current_streamed_bytes;
    m_args->prev_wall_s = now_wall_s;
    m_args->prev_cpu_s = current_cpu_s;
}

void *monitor_routine(void *arg) {
    MonitorArgs *m_args = (MonitorArgs *)arg;
    char rt_filepath[512];
    FILE *rt_fp;

    snprintf(rt_filepath, sizeof(rt_filepath), "%s/realtime.txt", m_args->log_task_dir);
    rt_fp = fopen(rt_filepath, "w");
    if (rt_fp) {
        fprintf(rt_fp, "RunTime(s),Process(%%),CPU(%%),SingleCoreCPU(%%),RSS(MB),Interval_TPS,Interval_BPS(Bytes/s),Cumul_TPS,Cumul_BPS(Bytes/s),Peak_TPS,Peak_BPS(Bytes/s),Success_Rate(%%),Total_Reqs\n");
        fflush(rt_fp);
    }

    m_args->start_wall_s = monotonic_seconds();
    m_args->prev_wall_s = m_args->start_wall_s;
    m_args->prev_cpu_s = process_cpu_seconds();
    m_args->logical_cpu_count = get_logical_cpu_count();

    while (!m_args->stop_flag && !g_graceful_stop) {
        int i;
        for (i = 0; i < m_args->interval_sec * 10 && !m_args->stop_flag && !g_graceful_stop; i++) {
            usleep(100000);
        }
        if (m_args->stop_flag || g_graceful_stop) break;
        emit_monitor_snapshot(m_args, rt_fp, 0);
    }

    emit_monitor_snapshot(m_args, rt_fp, 1);
    if (rt_fp) fclose(rt_fp);
    return NULL;
}

static void trim_newline(char *text) {
    size_t len;
    if (!text) return;
    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

static int run_capture_first_line(const char *cmd, char *out, size_t out_size) {
    FILE *fp;
    int status;

    if (!cmd || !out || out_size == 0) return -1;
    out[0] = '\0';

    fp = popen(cmd, "r");
    if (!fp) return -1;
    if (!fgets(out, (int)out_size, fp)) {
        pclose(fp);
        return -1;
    }
    trim_newline(out);
    status = pclose(fp);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static int run_command_status(const char *cmd) {
    int status = system(cmd);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int file_contains_text(const char *path, const char *needle) {
    FILE *fp;
    char line[512];
    if (!path || !needle) return 0;
    fp = fopen(path, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, needle)) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static void make_timestamp_id(const char *prefix, char *buf, size_t buf_size) {
    time_t now = time(NULL);
    struct tm t_res;
    struct tm *t = localtime_r(&now, &t_res);

    if (t) {
        snprintf(buf, buf_size, "%s_%04d%02d%02d_%02d%02d%02d",
                 prefix,
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        snprintf(buf, buf_size, "%s_UNKNOWN", prefix);
    }
}

static int parse_optional_int(const char *text, int *is_set) {
    if (!text || text[0] == '\0') {
        if (is_set) *is_set = 0;
        return 0;
    }
    if (is_set) *is_set = 1;
    return atoi(text);
}

static int parse_optional_bool(const char *text, int default_value) {
    if (!text || text[0] == '\0') return default_value;
    if (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0 || strcasecmp(text, "yes") == 0) return 1;
    if (strcmp(text, "0") == 0 || strcasecmp(text, "false") == 0 || strcasecmp(text, "no") == 0) return 0;
    return default_value;
}

static int load_suite_scenarios(const char *manifest_path, SuiteScenario **out_scenarios, int *out_count) {
    FILE *fp;
    char line[4096];
    int count = 0;
    int capacity = 16;
    SuiteScenario *items = NULL;

    if (!manifest_path || !out_scenarios || !out_count) return -1;
    fp = fopen(manifest_path, "r");
    if (!fp) return -1;

    items = (SuiteScenario *)calloc(capacity, sizeof(SuiteScenario));
    if (!items) {
        fclose(fp);
        return -1;
    }

    if (!fgets(line, sizeof(line), fp)) {
        free(items);
        fclose(fp);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *fields[21] = {0};
        char *cursor = line;
        int field_count = 0;
        SuiteScenario *scenario;

        trim_newline(line);
        if (line[0] == '\0') continue;

        while (field_count < 21) {
            char *tab = strchr(cursor, '\t');
            fields[field_count++] = cursor;
            if (!tab) break;
            *tab = '\0';
            cursor = tab + 1;
        }
        if (field_count < 21) continue;

        if (count >= capacity) {
            SuiteScenario *grown;
            capacity *= 2;
            grown = (SuiteScenario *)realloc(items, capacity * sizeof(SuiteScenario));
            if (!grown) {
                free(items);
                fclose(fp);
                return -1;
            }
            items = grown;
        }

        scenario = &items[count];
        memset(scenario, 0, sizeof(*scenario));
        snprintf(scenario->suite_id, sizeof(scenario->suite_id), "%s", fields[0]);
        snprintf(scenario->scenario_id, sizeof(scenario->scenario_id), "%s", fields[1]);
        snprintf(scenario->profile, sizeof(scenario->profile), "%s", fields[2]);
        snprintf(scenario->config_file, sizeof(scenario->config_file), "%s", fields[3]);
        snprintf(scenario->users_file, sizeof(scenario->users_file), "%s", fields[4]);
        snprintf(scenario->op, sizeof(scenario->op), "%s", fields[5]);
        scenario->threads = atoi(fields[6]);
        snprintf(scenario->object_size_spec, sizeof(scenario->object_size_spec), "%s", fields[7]);
        scenario->run_seconds = parse_optional_int(fields[8], &scenario->run_seconds_set);
        scenario->requests_per_thread = parse_optional_int(fields[9], &scenario->requests_per_thread_set);
        scenario->allow_open_ended_run = parse_optional_bool(fields[10], 0);
        scenario->continue_on_fail = parse_optional_bool(fields[11], 1);
        scenario->analyze_longrun = parse_optional_bool(fields[12], 0);
        scenario->gate_longrun = parse_optional_bool(fields[13], 0);
        snprintf(scenario->longrun_policy, sizeof(scenario->longrun_policy), "%s", fields[14]);
        snprintf(scenario->baseline_mode, sizeof(scenario->baseline_mode), "%s", fields[15]);
        scenario->baseline_enabled = parse_optional_bool(fields[16], 0);
        snprintf(scenario->baseline_policy, sizeof(scenario->baseline_policy), "%s", fields[17]);
        snprintf(scenario->baseline_manifest, sizeof(scenario->baseline_manifest), "%s", fields[18]);
        snprintf(scenario->baseline_store_dir, sizeof(scenario->baseline_store_dir), "%s", fields[19]);
        snprintf(scenario->baseline_update_strategy, sizeof(scenario->baseline_update_strategy), "%s", fields[20]);
        count++;
    }

    fclose(fp);
    *out_scenarios = items;
    *out_count = count;
    return 0;
}

static void cleanup_config_resources(Config *cfg) {
    int i;
    if (!cfg) return;
    if (cfg->user_list) {
        free(cfg->user_list);
        cfg->user_list = NULL;
    }
    for (i = 0; i < cfg->range_count; i++) {
        if (cfg->range_options[i]) {
            free(cfg->range_options[i]);
            cfg->range_options[i] = NULL;
        }
    }
}

static int parse_cli_options(int argc, char **argv, CliOptions *cli) {
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"users", required_argument, 0, 'u'},
        {"op", required_argument, 0, 'o'},
        {"threads", required_argument, 0, 't'},
        {"object-size", required_argument, 0, 's'},
        {"scenario-id", required_argument, 0, 'S'},
        {"output-dir", required_argument, 0, 'O'},
        {"log-dir", required_argument, 0, 'L'},
        {"suite", required_argument, 0, 'q'},
        {0, 0, 0, 0}
    };
    int opt;

    memset(cli, 0, sizeof(*cli));
    snprintf(cli->config_file, sizeof(cli->config_file), "config.dat");
    snprintf(cli->users_file, sizeof(cli->users_file), "users.dat");
    snprintf(cli->output_dir, sizeof(cli->output_dir), "reports");
    snprintf(cli->log_dir, sizeof(cli->log_dir), "logs");

    while ((opt = getopt_long(argc, argv, "c:u:o:t:s:S:O:L:q:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                snprintf(cli->config_file, sizeof(cli->config_file), "%s", optarg);
                break;
            case 'u':
                snprintf(cli->users_file, sizeof(cli->users_file), "%s", optarg);
                break;
            case 'o':
                if (parse_test_case_arg(optarg, &cli->op) != 0) {
                    fprintf(stderr, "Invalid --op value: %s\n", optarg);
                    return -1;
                }
                cli->op_set = 1;
                break;
            case 't':
                cli->threads = atoi(optarg);
                if (cli->threads <= 0) {
                    fprintf(stderr, "Invalid --threads value: %s\n", optarg);
                    return -1;
                }
                cli->threads_set = 1;
                break;
            case 's':
                snprintf(cli->object_size_spec, sizeof(cli->object_size_spec), "%s", optarg);
                cli->object_size_set = 1;
                break;
            case 'S':
                snprintf(cli->scenario_id, sizeof(cli->scenario_id), "%s", optarg);
                break;
            case 'O':
                snprintf(cli->output_dir, sizeof(cli->output_dir), "%s", optarg);
                cli->output_dir_set = 1;
                break;
            case 'L':
                snprintf(cli->log_dir, sizeof(cli->log_dir), "%s", optarg);
                cli->log_dir_set = 1;
                break;
            case 'q':
                snprintf(cli->suite_file, sizeof(cli->suite_file), "%s", optarg);
                cli->suite_mode = 1;
                break;
            default:
                return -1;
        }
    }

    while (optind < argc) {
        int parsed_case = 0;
        if (cli->suite_mode) {
            fprintf(stderr, "Unexpected positional argument in suite mode: %s\n", argv[optind]);
            return -1;
        } else if (!cli->op_set && parse_test_case_arg(argv[optind], &parsed_case) == 0) {
            cli->op = parsed_case;
            cli->op_set = 1;
        } else if (strcmp(cli->config_file, "config.dat") == 0) {
            snprintf(cli->config_file, sizeof(cli->config_file), "%s", argv[optind]);
        } else {
            fprintf(stderr, "Unexpected positional argument: %s\n", argv[optind]);
            return -1;
        }
        optind++;
    }

    return 0;
}

static void apply_cli_overrides(Config *cfg, const CliOptions *cli) {
    if (cli->op_set) {
        cfg->test_case = cli->op;
        cfg->use_mix_mode = (cfg->test_case == TEST_CASE_MIX && cfg->mix_op_count > 0) ? 1 : 0;
    } else if (cfg->test_case != TEST_CASE_MIX) {
        cfg->use_mix_mode = 0;
    }

    if (cli->threads_set) {
        cfg->threads = cli->threads;
        cfg->threads_source = CONFIG_SOURCE_CLI;
    }
    if (cli->object_size_set) {
        char errbuf[128] = {0};
        if (parse_object_size_spec(cli->object_size_spec, cfg, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "%s\n", errbuf);
            exit(1);
        }
        cfg->object_size_source = CONFIG_SOURCE_CLI;
    }

    snprintf(cfg->config_file_path, sizeof(cfg->config_file_path), "%s", cli->config_file);
    if (!cfg->is_temporary_token) {
        snprintf(cfg->users_file_path, sizeof(cfg->users_file_path), "%s", cli->users_file);
    }
    if (cli->scenario_id[0] != '\0') {
        snprintf(cfg->scenario_id, sizeof(cfg->scenario_id), "%s", cli->scenario_id);
    }
    detect_host_metadata(cfg);
    load_git_commit_metadata(cfg);
}

static void apply_suite_scenario_overrides(Config *cfg, const CliOptions *cli, const SuiteScenario *scenario) {
    char errbuf[128] = {0};
    int scenario_case = 0;

    if (parse_test_case_arg(scenario->op, &scenario_case) != 0) {
        fprintf(stderr, "Invalid scenario op value: %s\n", scenario->op);
        exit(1);
    }

    cfg->test_case = scenario_case;
    cfg->use_mix_mode = (cfg->test_case == TEST_CASE_MIX && cfg->mix_op_count > 0) ? 1 : 0;
    cfg->threads = scenario->threads;
    cfg->threads_source = CONFIG_SOURCE_SUITE;
    if (parse_object_size_spec(scenario->object_size_spec, cfg, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "%s\n", errbuf);
        exit(1);
    }
    cfg->object_size_source = CONFIG_SOURCE_SUITE;
    if (scenario->run_seconds_set) {
        cfg->run_seconds = scenario->run_seconds;
        cfg->run_seconds_source = CONFIG_SOURCE_SUITE;
    }
    if (scenario->requests_per_thread_set) {
        cfg->requests_per_thread = scenario->requests_per_thread;
        cfg->requests_per_thread_source = CONFIG_SOURCE_SUITE;
    }
    cfg->allow_open_ended_run = scenario->allow_open_ended_run;
    cfg->allow_open_ended_run_source = CONFIG_SOURCE_SUITE;
    cfg->analyze_longrun = scenario->analyze_longrun;
    cfg->gate_longrun = scenario->gate_longrun;

    snprintf(cfg->config_file_path, sizeof(cfg->config_file_path), "%s", scenario->config_file);
    if (!cfg->is_temporary_token) {
        const char *users_path = scenario->users_file[0] ? scenario->users_file : cli->users_file;
        snprintf(cfg->users_file_path, sizeof(cfg->users_file_path), "%s", users_path);
    }
    snprintf(cfg->scenario_id, sizeof(cfg->scenario_id), "%s", scenario->scenario_id);
    detect_host_metadata(cfg);
    load_git_commit_metadata(cfg);
}

static const char *task_id_from_dir(const char *task_dir) {
    const char *slash = strrchr(task_dir, '/');
    return slash ? slash + 1 : task_dir;
}

static double compute_p99_latency(const unsigned long long *hist, long long sample_count) {
    unsigned long long cumulative = 0;
    unsigned long long target;
    int bucket;

    if (sample_count <= 0) return 0.0;
    target = (unsigned long long)((sample_count * 99LL + 99LL) / 100LL);
    for (bucket = 0; bucket < LATENCY_HIST_BUCKETS; bucket++) {
        cumulative += hist[bucket];
        if (cumulative >= target) {
            return latency_bucket_upper_bound_ms(bucket);
        }
    }
    return latency_bucket_upper_bound_ms(LATENCY_HIST_BUCKETS - 1);
}

static void save_archive_csv(Config *cfg, const BenchmarkSummary *summary) {
    char filepath[512];
    FILE *fp;
    double threads_per_user_effective = cfg->loaded_user_count > 0 ?
        (double)cfg->threads / cfg->loaded_user_count : 0.0;
    time_t now = time(NULL);
    struct tm t_res;
    struct tm *t = localtime_r(&now, &t_res);
    char timebuf[64] = {0};

    if (t) {
        snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    }

    snprintf(filepath, sizeof(filepath), "%s/archive.csv", cfg->report_task_dir);
    fp = fopen(filepath, "w");
    if (!fp) return;

    fprintf(fp, "task_id,start_time,config_file,users_file,op,users_loaded,total_threads,threads_per_user_effective,object_size_spec,actual_duration_s,total_requests,success_requests,failed_requests,success_rate_pct,avg_cpu_pct,peak_cpu_pct,avg_single_core_cpu_pct,peak_single_core_cpu_pct,avg_rss_mb,peak_rss_mb,final_tps,peak_tps,final_bps_bytes_per_sec,peak_bps_bytes_per_sec,avg_latency_ms,p99_latency_ms,avg_single_stream_bps,max_single_stream_bps,scenario_id,git_commit,host_os,host_arch\n");
    fprintf(fp, "%s,%s,%s,%s,%s,%d,%d,%.2f,%s,%.6f,%lld,%lld,%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%s,%s,%s,%s\n",
            task_id_from_dir(cfg->report_task_dir),
            timebuf,
            cfg->config_file_path,
            cfg->users_file_path,
            test_case_to_name(cfg->test_case),
            cfg->loaded_user_count,
            cfg->threads,
            threads_per_user_effective,
            cfg->object_size_spec,
            summary->actual_duration_s,
            summary->total_requests,
            summary->success_requests,
            summary->failed_requests,
            summary->success_rate_pct,
            summary->avg_cpu_pct,
            summary->peak_cpu_pct,
            summary->avg_single_core_cpu_pct,
            summary->peak_single_core_cpu_pct,
            summary->avg_rss_mb,
            summary->peak_rss_mb,
            summary->final_tps,
            summary->peak_tps,
            summary->final_bps,
            summary->peak_bps,
            summary->avg_latency_ms,
            summary->p99_latency_ms,
            summary->avg_single_stream_bps,
            summary->max_single_stream_bps,
            cfg->scenario_id,
            cfg->git_commit,
            cfg->host_os,
            cfg->host_arch);
    fclose(fp);
}

void save_benchmark_report(Config *cfg, const BenchmarkSummary *summary) {
    char filepath[512];
    FILE *fp;
    time_t now = time(NULL);
    struct tm t_res;
    struct tm *t = localtime_r(&now, &t_res);
    double threads_per_user_effective = cfg->loaded_user_count > 0 ?
        (double)cfg->threads / cfg->loaded_user_count : 0.0;
    char duration_human[64];
    char object_size_human[64];
    char object_size_min_human[64];
    char object_size_max_human[64];
    char part_size_human[64];
    char final_bps_human[64];
    char peak_bps_human[64];
    char avg_single_stream_human[64];
    char max_single_stream_human[64];
    char avg_latency_human[64];
    char p99_latency_human[64];
    char avg_rss_human[64];
    char peak_rss_human[64];

    snprintf(filepath, sizeof(filepath), "%s/brief.txt", cfg->report_task_dir);
    fp = fopen(filepath, "w");
    if (!fp) return;

    format_duration_human(summary->actual_duration_s, duration_human, sizeof(duration_human));
    format_bytes_si(cfg->object_size_max, object_size_human, sizeof(object_size_human));
    format_bytes_si(cfg->object_size_min, object_size_min_human, sizeof(object_size_min_human));
    format_bytes_si(cfg->object_size_max, object_size_max_human, sizeof(object_size_max_human));
    format_bytes_si(cfg->part_size, part_size_human, sizeof(part_size_human));
    format_rate_si(summary->final_bps, final_bps_human, sizeof(final_bps_human));
    format_rate_si(summary->peak_bps, peak_bps_human, sizeof(peak_bps_human));
    format_rate_si(summary->avg_single_stream_bps, avg_single_stream_human, sizeof(avg_single_stream_human));
    format_rate_si(summary->max_single_stream_bps, max_single_stream_human, sizeof(max_single_stream_human));
    format_latency_human(summary->avg_latency_ms, avg_latency_human, sizeof(avg_latency_human));
    format_latency_human(summary->p99_latency_ms, p99_latency_human, sizeof(p99_latency_human));
    format_memory_mb_human(summary->avg_rss_mb, avg_rss_human, sizeof(avg_rss_human));
    format_memory_mb_human(summary->peak_rss_mb, peak_rss_human, sizeof(peak_rss_human));

    fprintf(fp, "===========================================\n");
    fprintf(fp, "      OBS C SDK Benchmark Execution Report \n");
    fprintf(fp, "===========================================\n");
    if (t) {
        fprintf(fp, "Execution Time:      %04d-%02d-%02d %02d:%02d:%02d\n",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                t->tm_hour, t->tm_min, t->tm_sec);
    }

    fprintf(fp, "---------------- Configuration ----------------\n");
    fprintf(fp, "[Environment]\n");
    fprintf(fp, "  Endpoint:          %s\n", cfg->endpoint);
    fprintf(fp, "  LogLevel:          %s\n", log_level_to_string(cfg->log_level));
    fprintf(fp, "  ConfigFile:        %s\n", cfg->config_file_path);
    fprintf(fp, "  UsersFile:         %s\n", cfg->users_file_path);
    fprintf(fp, "  ScenarioID:        %s\n", cfg->scenario_id);
    fprintf(fp, "  GitCommit:         %s\n", cfg->git_commit);
    fprintf(fp, "  HostOS:            %s\n", cfg->host_os);
    fprintf(fp, "  HostArch:          %s\n", cfg->host_arch);
    fprintf(fp, "  ReportDir:         %s\n", cfg->report_task_dir);
    fprintf(fp, "  LogDir:            %s\n", cfg->log_task_dir);
    fprintf(fp, "  EffectiveConfig:   %s\n", cfg->effective_config_path[0] ? cfg->effective_config_path : "N/A");
    fprintf(fp, "  Bucket(Fixed):     %s\n", cfg->bucket_name_fixed[0] ? cfg->bucket_name_fixed : "N/A");
    fprintf(fp, "  Bucket(Prefix):    %s\n", cfg->bucket_name_prefix[0] ? cfg->bucket_name_prefix : "N/A");
    fprintf(fp, "  STS Auth Mode:     %s\n", cfg->is_temporary_token ? "true" : "false");

    fprintf(fp, "[Network]\n");
    fprintf(fp, "  Protocol:          %s\n", cfg->protocol);
    fprintf(fp, "  KeepAlive:         %s\n", cfg->keep_alive ? "true" : "false");
    fprintf(fp, "  ConnectTimeout:    %d sec\n", cfg->connect_timeout_sec);
    fprintf(fp, "  RequestTimeout:    %d sec\n", cfg->request_timeout_sec);
    fprintf(fp, "  Gm Auth Mode:      %s\n", cfg->gm_auth_mode[0] ? cfg->gm_auth_mode : "Default");

    fprintf(fp, "[TestPlan]\n");
    fprintf(fp, "  Operation:         %s (%d)\n", test_case_to_name(cfg->test_case), cfg->test_case);
    fprintf(fp, "  ExecutionMode:     %s\n", execution_mode_label(cfg));
    fprintf(fp, "  Total Threads:     %d\n", cfg->threads);
    fprintf(fp, "  Loaded Users:      %d\n", cfg->loaded_user_count);
    fprintf(fp, "  Avg Threads/User:  %.2f\n", threads_per_user_effective);
    fprintf(fp, "  RunSeconds:        %d %s\n", cfg->run_seconds, cfg->run_seconds > 0 ? "(Time Limited)" : "(No Limit)");
    fprintf(fp, "  Reqs/Thread:       %d\n", cfg->requests_per_thread);
    fprintf(fp, "  LongrunEnabled:    %s\n", (cfg->analyze_longrun || cfg->gate_longrun) ? "true" : "false");
    fprintf(fp, "  OpenEndedRun:      %s\n", cfg->allow_open_ended_run ? "ENABLED (explicit override)" : "disabled");

    fprintf(fp, "[ObjectSettings]\n");
    fprintf(fp, "  ObjectSizeSpec:    %s\n", cfg->object_size_spec);
    if (cfg->is_dynamic_size) {
        fprintf(fp, "  ObjectSize:        %s ~ %s (%lld ~ %lld Bytes, Dynamic)\n",
                object_size_min_human, object_size_max_human, cfg->object_size_min, cfg->object_size_max);
    } else {
        fprintf(fp, "  ObjectSize:        %s (%lld Bytes)\n", object_size_human, cfg->object_size_max);
    }
    fprintf(fp, "  PartSize:          %s (%lld Bytes)\n", part_size_human, cfg->part_size);
    fprintf(fp, "  Parts/Upload:      %d\n", cfg->parts_for_each_upload_id);
    fprintf(fp, "  KeyPrefix:         %s\n", cfg->key_prefix);
    fprintf(fp, "  KeyHashPrefix:     %s\n", cfg->obj_name_pattern_hash ? "true" : "false");

    fprintf(fp, "[Resumable & Validation]\n");
    fprintf(fp, "  EnableCheckpoint:  %s\n", cfg->enable_checkpoint ? "true" : "false");
    fprintf(fp, "  ResumableTaskNum:  %d\n", cfg->resumable_task_num);
    fprintf(fp, "  UploadFilePath:    %s\n", cfg->upload_file_path[0] ? cfg->upload_file_path : "N/A");
    fprintf(fp, "  DataValidation:    %s\n", cfg->enable_data_validation ? "true" : "false");

    fprintf(fp, "---------------- Statistics -------------------\n");
    fprintf(fp, "Total Requests:      %lld\n", summary->total_requests);
    fprintf(fp, "Success:             %lld\n", summary->success_requests);
    fprintf(fp, "Failed:              %lld\n", summary->failed_requests);
    fprintf(fp, "  |- 403 (Forbidden):  %lld\n", summary->fail_403);
    fprintf(fp, "  |- 404 (NotFound):   %lld\n", summary->fail_404);
    fprintf(fp, "  |- 409 (Conflict):   %lld\n", summary->fail_409);
    fprintf(fp, "  |- 4xx (Other):      %lld\n", summary->fail_4xx_other);
    fprintf(fp, "  |- 5xx (Server):     %lld\n", summary->fail_5xx);
    fprintf(fp, "  |- Other (Net/SDK):  %lld\n", summary->fail_other);
    fprintf(fp, "  |- Internal Validation Fail: %lld\n", summary->fail_validation);

    fprintf(fp, "\nPerformance:\n");
    fprintf(fp, "  Actual Duration:      %s (%.4f s)\n", duration_human, summary->actual_duration_s);
    fprintf(fp, "  Success Rate:         %.4f %%\n", summary->success_rate_pct);
    fprintf(fp, "  Final TPS:            %.4f\n", summary->final_tps);
    fprintf(fp, "  Peak TPS:             %.4f\n", summary->peak_tps);
    fprintf(fp, "  Final BPS:            %s (%.4f Bytes/s)\n", final_bps_human, summary->final_bps);
    fprintf(fp, "  Peak BPS:             %s (%.4f Bytes/s)\n", peak_bps_human, summary->peak_bps);
    fprintf(fp, "  Avg Latency:          %s (%.4f ms)\n", avg_latency_human, summary->avg_latency_ms);
    fprintf(fp, "  P99 Latency:          %s (%.4f ms)\n", p99_latency_human, summary->p99_latency_ms);
    fprintf(fp, "  Avg Single Stream:    %s (%.4f Bytes/s)\n", avg_single_stream_human, summary->avg_single_stream_bps);
    fprintf(fp, "  Max Single Stream:    %s (%.4f Bytes/s)\n", max_single_stream_human, summary->max_single_stream_bps);
    fprintf(fp, "  Avg CPU:              %.4f %%\n", summary->avg_cpu_pct);
    fprintf(fp, "  Peak CPU:             %.4f %%\n", summary->peak_cpu_pct);
    fprintf(fp, "  Avg Single-Core CPU:  %.4f %%\n", summary->avg_single_core_cpu_pct);
    fprintf(fp, "  Peak Single-Core CPU: %.4f %%\n", summary->peak_single_core_cpu_pct);
    fprintf(fp, "  Avg RSS:              %s (%.4f MB)\n", avg_rss_human, summary->avg_rss_mb);
    fprintf(fp, "  Peak RSS:             %s (%.4f MB)\n", peak_rss_human, summary->peak_rss_mb);
    fprintf(fp, "===========================================\n");

    fclose(fp);
    LOG_INFO("Execution report saved to: %s", filepath);
}

static int run_benchmark_scenario(const CliOptions *cli,
                                  const SuiteScenario *scenario,
                                  const char *report_dir,
                                  const char *log_dir,
                                  ScenarioRunResult *result) {
    Config cfg;
    pthread_t *tids = NULL;
    WorkerArgs *t_args = NULL;
    pthread_t monitor_tid;
    MonitorArgs m_args;
    BenchmarkSummary summary;
    struct timeval main_start_tv, main_end_tv;
    double current_ms = 0.0;
    double stop_ms = 0.0;
    obs_status status;
    unsigned long long merged_hist[LATENCY_HIST_BUCKETS] = {0};
    long long total_latency_samples = 0;
    long long total_completed_bytes = 0;
    double single_stream_sum = 0.0;
    int single_stream_count = 0;
    int global_thread_idx = 0;
    int using_explicit_total_threads = 0;
    int u;
    int exit_code = 1;
    int obs_initialized = 0;
    const char *config_path = scenario ? scenario->config_file : cli->config_file;
    char validation_error[512];

    if (result) {
        memset(result, 0, sizeof(*result));
        result->exit_status = 1;
        result->longrun_exit_status = 0;
        result->effective_run_seconds = 0;
        result->effective_requests_per_thread = 0;
        result->analyze_longrun = 0;
        result->gate_longrun = 0;
        snprintf(result->report_dir, sizeof(result->report_dir), "%s", report_dir);
        snprintf(result->log_dir, sizeof(result->log_dir), "%s", log_dir);
    }

    initialize_break_point_lock();
    memset(&cfg, 0, sizeof(cfg));
    memset(&summary, 0, sizeof(summary));
    memset(&m_args, 0, sizeof(m_args));

    if (load_config(config_path, &cfg) != 0) goto cleanup;
    if (scenario) apply_suite_scenario_overrides(&cfg, cli, scenario);
    else apply_cli_overrides(&cfg, cli);

    snprintf(cfg.report_task_dir, sizeof(cfg.report_task_dir), "%s", report_dir);
    snprintf(cfg.log_task_dir, sizeof(cfg.log_task_dir), "%s", log_dir);
    if (ensure_directory(cfg.report_task_dir) != 0) {
        fprintf(stderr, "Failed to create report output directory: %s\n", cfg.report_task_dir);
        goto cleanup;
    }
    if (ensure_directory(cfg.log_task_dir) != 0) {
        fprintf(stderr, "Failed to create log output directory: %s\n", cfg.log_task_dir);
        goto cleanup;
    }

    log_init(cfg.log_level);
    LOG_INFO("--- OBS C SDK Benchmark Tool ---");
    LOG_INFO("Report Output Dir: %s", cfg.report_task_dir);
    LOG_INFO("Log Output Dir: %s", cfg.log_task_dir);

    if (cfg.is_temporary_token) {
        char cmd[PATH_MAX * 2];
        LOG_INFO("IsTemporaryToken enabled. Fetching STS tokens for %d users...", cfg.target_user_count);
        snprintf(cmd, sizeof(cmd), "python3 scripts/sdk/generate_temp_ak_sk.py %d", cfg.target_user_count);
        if (run_command_status(cmd) != 0) {
            LOG_ERROR("FATAL: Failed to generate temporary credentials. (Command: %s)", cmd);
            goto cleanup;
        }
        snprintf(cfg.users_file_path, sizeof(cfg.users_file_path), "%s", "temptoken.dat");
        if (load_users_file(cfg.users_file_path, &cfg, 1) < 0) goto cleanup;
    } else {
        const char *users_path = scenario && scenario->users_file[0] ? scenario->users_file : cli->users_file;
        snprintf(cfg.users_file_path, sizeof(cfg.users_file_path), "%s", users_path);
        if (load_users_file(cfg.users_file_path, &cfg, 0) < 0) goto cleanup;
    }

    if (cfg.loaded_user_count <= 0) {
        LOG_ERROR("No users loaded from %s", cfg.users_file_path);
        goto cleanup;
    }

    using_explicit_total_threads = (scenario != NULL || cli->threads_set);
    if (scenario) {
        cfg.threads = scenario->threads;
    } else if (!cli->threads_set) {
        cfg.threads = cfg.loaded_user_count * cfg.threads_per_user;
        cfg.threads_source = CONFIG_SOURCE_CONFIG;
    } else {
        cfg.threads = cli->threads;
        cfg.threads_source = CONFIG_SOURCE_CLI;
    }
    if (cfg.scenario_id[0] == '\0') {
        infer_scenario_id(&cfg, cfg.scenario_id, sizeof(cfg.scenario_id));
    }
    if (result) {
        result->effective_run_seconds = cfg.run_seconds;
        result->effective_requests_per_thread = cfg.requests_per_thread;
        result->analyze_longrun = cfg.analyze_longrun;
        result->gate_longrun = cfg.gate_longrun;
    }

    printf("[Config] Multi-User Mode: %d Users Loaded. Total Threads: %d\n", cfg.loaded_user_count, cfg.threads);
    printf("[Config] Operation: %s (%d)\n", test_case_to_name(cfg.test_case), cfg.test_case);
    printf("[Config] ObjectSize: %s\n", cfg.object_size_spec);
    printf("[Config] ExecutionMode: %s\n", execution_mode_label(&cfg));
    printf("[Config] Effective RunSeconds: %d\n", cfg.run_seconds);
    printf("[Config] Effective RequestsPerThread: %d\n", cfg.requests_per_thread);
    printf("[Config] LongrunEnabled: %s\n", (cfg.analyze_longrun || cfg.gate_longrun) ? "true" : "false");
    if (scenario) printf("[Config] Suite Scenario: %s (%s)\n", scenario->scenario_id, scenario->profile);
    if (cfg.allow_open_ended_run && strcmp(execution_mode_label(&cfg), "Open-Ended") == 0) {
        printf("[Config] OpenEndedRun: ENABLED (explicit override)\n");
        printf("[WARN] This scenario has no automatic stop condition and must be stopped manually.\n");
    }
    if (cfg.enable_data_validation) printf("[Config] Data Validation: ENABLED\n");
    if (cfg.enable_detail_log) printf("[Config] Detail Request Log: ENABLED\n");
    emit_irrelevant_config_warnings(&cfg);

    snprintf(cfg.effective_config_path, sizeof(cfg.effective_config_path), "%s/effective_config.md", cfg.report_task_dir);
    if (write_effective_config_report(&cfg) != 0) {
        LOG_ERROR("Failed to write effective config report: %s", cfg.effective_config_path);
        goto cleanup;
    }
    printf("[Config] EffectiveConfig: %s\n", cfg.effective_config_path);

    if (validate_runtime_config(&cfg, using_explicit_total_threads, validation_error, sizeof(validation_error)) != 0) {
        LOG_ERROR("[Config Error] %s", validation_error);
        goto cleanup;
    }

    status = obs_initialize(OBS_INIT_ALL);
    if (status != OBS_STATUS_OK) goto cleanup;
    obs_initialized = 1;

    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        current_ms = ts_now.tv_sec * 1000.0 + ts_now.tv_nsec / 1000000.0;
    }
    stop_ms = (cfg.run_seconds > 0) ? (current_ms + cfg.run_seconds * 1000.0) : 1e15;

    tids = (pthread_t *)malloc(cfg.threads * sizeof(pthread_t));
    t_args = (WorkerArgs *)calloc(cfg.threads, sizeof(WorkerArgs));
    if (!tids || !t_args) {
        LOG_ERROR("Failed to allocate thread resources");
        goto cleanup;
    }

    gettimeofday(&main_start_tv, NULL);

    for (u = 0; u < cfg.loaded_user_count; u++) {
        UserCredential *curr_user = &cfg.user_list[u];
        char target_bucket[256] = {0};
        int threads_for_user;
        int t_idx;

        if (strlen(cfg.bucket_name_fixed) > 0) {
            strcpy(target_bucket, cfg.bucket_name_fixed);
        } else {
            char ak_lower[128] = {0};
            if (strlen(curr_user->original_ak) > 0) {
                str_tolower(ak_lower, curr_user->original_ak);
            }

            if (strlen(cfg.bucket_name_prefix) > 0) {
                if (strlen(ak_lower) > 0) snprintf(target_bucket, sizeof(target_bucket), "%s.%s", ak_lower, cfg.bucket_name_prefix);
                else snprintf(target_bucket, sizeof(target_bucket), "%s", cfg.bucket_name_prefix);
            } else if (strlen(ak_lower) > 0) {
                snprintf(target_bucket, sizeof(target_bucket), "%s", ak_lower);
            } else {
                strcpy(target_bucket, "default-bench-bucket");
            }
        }

        if (scenario || cli->threads_set) {
            int base_threads = cfg.threads / cfg.loaded_user_count;
            int remainder = cfg.threads % cfg.loaded_user_count;
            threads_for_user = base_threads + (u < remainder ? 1 : 0);
        } else {
            threads_for_user = cfg.threads_per_user;
        }

        for (t_idx = 0; t_idx < threads_for_user; t_idx++) {
            WorkerArgs *args;
            if (global_thread_idx >= cfg.threads) break;
            args = &t_args[global_thread_idx];
            args->thread_id = global_thread_idx;
            args->config = &cfg;
            args->stop_timestamp_ms = stop_ms;
            strcpy(args->effective_ak, curr_user->ak);
            strcpy(args->effective_sk, curr_user->sk);
            strcpy(args->effective_bucket, target_bucket);
            strcpy(args->username, curr_user->username);

            if (cfg.is_temporary_token) {
                strcpy(args->effective_token, curr_user->security_token);
            } else {
                memset(args->effective_token, 0, sizeof(args->effective_token));
            }

            pthread_create(&tids[global_thread_idx], NULL, worker_routine, args);
            global_thread_idx++;
        }
    }

    m_args.t_args = t_args;
    m_args.thread_count = cfg.threads;
    m_args.interval_sec = 3;
    m_args.stop_flag = 0;
    strcpy(m_args.log_task_dir, cfg.log_task_dir);

    pthread_create(&monitor_tid, NULL, monitor_routine, &m_args);

    for (u = 0; u < cfg.threads; u++) {
        pthread_join(tids[u], NULL);
    }

    m_args.stop_flag = 1;
    pthread_join(monitor_tid, NULL);

    gettimeofday(&main_end_tv, NULL);
    summary.actual_duration_s = (main_end_tv.tv_sec - main_start_tv.tv_sec) +
                                (main_end_tv.tv_usec - main_start_tv.tv_usec) / 1000000.0;

    for (u = 0; u < cfg.threads; u++) {
        int bucket;
        double thread_duration = t_args[u].worker_end_time_s - t_args[u].worker_start_time_s;

        summary.success_requests += t_args[u].stats.success_count;
        total_completed_bytes += t_args[u].stats.completed_success_bytes;
        summary.fail_403 += t_args[u].stats.fail_403_count;
        summary.fail_404 += t_args[u].stats.fail_404_count;
        summary.fail_409 += t_args[u].stats.fail_409_count;
        summary.fail_4xx_other += t_args[u].stats.fail_4xx_other_count;
        summary.fail_5xx += t_args[u].stats.fail_5xx_count;
        summary.fail_other += t_args[u].stats.fail_other_count;
        summary.fail_validation += t_args[u].stats.fail_validation_count;
        summary.avg_latency_ms += t_args[u].stats.total_latency_ms;
        total_latency_samples += t_args[u].stats.latency_sample_count;

        for (bucket = 0; bucket < LATENCY_HIST_BUCKETS; bucket++) {
            merged_hist[bucket] += t_args[u].stats.latency_hist[bucket];
        }

        if (thread_duration <= 0.0) thread_duration = summary.actual_duration_s > 0.0 ? summary.actual_duration_s : 0.000001;
        {
            double single_stream_bps = t_args[u].stats.completed_success_bytes / thread_duration;
            single_stream_sum += single_stream_bps;
            single_stream_count++;
            if (single_stream_bps > summary.max_single_stream_bps) summary.max_single_stream_bps = single_stream_bps;
        }
    }

    summary.failed_requests = summary.fail_403 + summary.fail_404 + summary.fail_409 +
                              summary.fail_4xx_other + summary.fail_5xx +
                              summary.fail_other + summary.fail_validation;
    summary.total_requests = summary.success_requests + summary.failed_requests;
    summary.success_rate_pct = summary.total_requests > 0 ?
        ((double)summary.success_requests / summary.total_requests) * 100.0 : 0.0;
    summary.final_tps = summary.actual_duration_s > 0.0 ? summary.total_requests / summary.actual_duration_s : 0.0;
    summary.final_bps = summary.actual_duration_s > 0.0 ? total_completed_bytes / summary.actual_duration_s : 0.0;
    summary.peak_tps = m_args.peak_tps;
    summary.peak_bps = m_args.peak_bps;
    summary.avg_cpu_pct = m_args.cpu_sample_count > 0 ? m_args.cpu_sum_pct / m_args.cpu_sample_count : -1.0;
    summary.peak_cpu_pct = m_args.cpu_sample_count > 0 ? m_args.cpu_peak_pct : -1.0;
    summary.avg_single_core_cpu_pct = m_args.single_core_cpu_sample_count > 0 ?
        m_args.single_core_cpu_sum_pct / m_args.single_core_cpu_sample_count : -1.0;
    summary.peak_single_core_cpu_pct = m_args.single_core_cpu_sample_count > 0 ?
        m_args.single_core_cpu_peak_pct : -1.0;
    summary.avg_rss_mb = m_args.rss_sample_count > 0 ? m_args.rss_sum_mb / m_args.rss_sample_count : -1.0;
    summary.peak_rss_mb = m_args.rss_sample_count > 0 ? m_args.rss_peak_mb : -1.0;
    summary.avg_latency_ms = total_latency_samples > 0 ? summary.avg_latency_ms / total_latency_samples : 0.0;
    summary.p99_latency_ms = compute_p99_latency(merged_hist, total_latency_samples);
    summary.avg_single_stream_bps = single_stream_count > 0 ? single_stream_sum / single_stream_count : 0.0;

    if (g_graceful_stop) {
        printf("\n[WARN] Benchmark interrupted by user (Graceful Stop).\n");
    }

    printf("\n--- Test Result ---\n");
    printf("Actual Duration: %.4f s\n", summary.actual_duration_s);
    printf("Total Requests:  %lld\n", summary.total_requests);
    printf("Success:         %lld\n", summary.success_requests);
    printf("Failed:          %lld\n", summary.failed_requests);
    printf("  |- 403 (Forbidden):  %lld\n", summary.fail_403);
    printf("  |- 404 (NotFound):   %lld\n", summary.fail_404);
    printf("  |- 409 (Conflict):   %lld\n", summary.fail_409);
    printf("  |- 4xx (Other):      %lld\n", summary.fail_4xx_other);
    printf("  |- 5xx (Server):     %lld\n", summary.fail_5xx);
    printf("  |- Other (Net/SDK):  %lld\n", summary.fail_other);
    printf("  |- Internal Validation Fail: %lld\n", summary.fail_validation);
    printf("TPS:             %.4f\n", summary.final_tps);
    printf("BPS:             %.4f Bytes/s\n", summary.final_bps);
    printf("Latency Avg:     %.4f ms\n", summary.avg_latency_ms);
    printf("Latency P99:     %.4f ms\n", summary.p99_latency_ms);
    printf("Avg CPU:         %.4f %%\n", summary.avg_cpu_pct);
    printf("Peak CPU:        %.4f %%\n", summary.peak_cpu_pct);
    printf("Avg SingleCore:  %.4f %%\n", summary.avg_single_core_cpu_pct);
    printf("Peak SingleCore: %.4f %%\n", summary.peak_single_core_cpu_pct);
    printf("Avg RSS:         %.4f MB\n", summary.avg_rss_mb);
    printf("Peak RSS:        %.4f MB\n", summary.peak_rss_mb);
    printf("Avg SingleFlow:  %.4f Bytes/s\n", summary.avg_single_stream_bps);
    printf("Max SingleFlow:  %.4f Bytes/s\n", summary.max_single_stream_bps);

    save_benchmark_report(&cfg, &summary);
    save_archive_csv(&cfg, &summary);
    exit_code = 0;

cleanup:
    if (obs_initialized) {
        obs_deinitialize();
    }
    if (tids) free(tids);
    if (t_args) free(t_args);
    cleanup_config_resources(&cfg);
    deinitialize_break_point_lock();
    if (result) result->exit_status = exit_code;
    return exit_code;
}

static int write_suite_result_header(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "suite_id\trun_id\tscenario_id\tprofile\tconfig_file\tusers_file\top\tthreads\tobject_size_spec\trun_seconds\trequests_per_thread\tanalyze_longrun\tgate_longrun\treport_dir\tlog_dir\texit_status\tlongrun_exit_status\tbaseline_exit_status\tbaseline_mode\n");
    fclose(fp);
    return 0;
}

static int append_suite_result(const char *path,
                               const char *run_id,
                               const SuiteScenario *scenario,
                               const ScenarioRunResult *result) {
    FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    fprintf(fp, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%s\n",
            scenario->suite_id,
            run_id,
            scenario->scenario_id,
            scenario->profile,
            scenario->config_file,
            scenario->users_file,
            scenario->op,
            scenario->threads,
            scenario->object_size_spec,
            result->effective_run_seconds,
            result->effective_requests_per_thread,
            result->analyze_longrun,
            result->gate_longrun,
            result->report_dir,
            result->log_dir,
            result->exit_status,
            result->longrun_exit_status,
            result->baseline_exit_status,
            scenario->baseline_mode);
    fclose(fp);
    return 0;
}

static int run_longrun_analysis(const SuiteScenario *scenario, const ScenarioRunResult *result) {
    char cmd[PATH_MAX * 4];
    char realtime_path[PATH_MAX];
    char archive_path[PATH_MAX];

    snprintf(realtime_path, sizeof(realtime_path), "%s/realtime.txt", result->log_dir);
    snprintf(archive_path, sizeof(archive_path), "%s/archive.csv", result->report_dir);
    if (access(realtime_path, F_OK) != 0 || access(archive_path, F_OK) != 0) {
        return 2;
    }

    snprintf(cmd, sizeof(cmd),
             "python3 scripts/reporting/analyze_longrun.py --realtime \"%s\" --archive \"%s\" --output-dir \"%s\" --policy \"%s\"%s",
             realtime_path,
             archive_path,
             result->report_dir,
             scenario->longrun_policy[0] ? scenario->longrun_policy : "ci/perf/longrun_policy.yaml",
             scenario->gate_longrun ? " --gate" : "");
    return run_command_status(cmd);
}

static int run_perf_baseline_action(const SuiteScenario *scenario, const ScenarioRunResult *result, const char *run_id) {
    char cmd[PATH_MAX * 6];
    char archive_path[PATH_MAX];
    char perf_dir[PATH_MAX];

    snprintf(archive_path, sizeof(archive_path), "%s/archive.csv", result->report_dir);
    if (access(archive_path, F_OK) != 0) {
        return 2;
    }
    if (!scenario->baseline_enabled || scenario->baseline_mode[0] == '\0' || strcmp(scenario->baseline_mode, "off") == 0) {
        return 0;
    }

    snprintf(perf_dir, sizeof(perf_dir), "%s/perf_gate", result->report_dir);
    if (strcmp(scenario->baseline_mode, "generate") == 0) {
        snprintf(cmd, sizeof(cmd),
                 "python3 scripts/reporting/register_baseline.py --archive \"%s\" --manifest \"%s\" --store-dir \"%s\" --label \"%s\" --update-strategy \"%s\" --source-run \"%s\"",
                 archive_path,
                 scenario->baseline_manifest,
                 scenario->baseline_store_dir,
                 scenario->scenario_id,
                 scenario->baseline_update_strategy[0] ? scenario->baseline_update_strategy : "new_label",
                 run_id ? run_id : "");
        return run_command_status(cmd);
    }
    if (strcmp(scenario->baseline_mode, "compare") == 0) {
        snprintf(cmd, sizeof(cmd),
                 "python3 scripts/reporting/perf_gate.py --candidate \"%s\" --policy \"%s\" --baselines-manifest \"%s\" --scenario-id \"%s\" --output-dir \"%s\"",
                 archive_path,
                 scenario->baseline_policy[0] ? scenario->baseline_policy : "ci/perf/gate_policy.json",
                 scenario->baseline_manifest,
                 scenario->scenario_id,
                 perf_dir);
        return run_command_status(cmd);
    }
    return 2;
}

static int run_suite_mode(const CliOptions *cli) {
    char suite_id[128];
    char run_id[64];
    char suite_report_root[PATH_MAX];
    char suite_log_root[PATH_MAX];
    char summary_dir[PATH_MAX];
    char resolved_tsv[PATH_MAX];
    char resolved_yaml[PATH_MAX];
    char results_tsv[PATH_MAX];
    char cmd[PATH_MAX * 4];
    SuiteScenario *scenarios = NULL;
    int scenario_count = 0;
    int i;
    int overall_exit = 0;

    printf("[Suite] Execution model: single process, sequential scenarios\n");

    if (run_capture_first_line(
            (snprintf(cmd, sizeof(cmd),
                      "python3 scripts/suites/resolve_suite.py --suite \"%s\" --print-suite-id",
                      cli->suite_file), cmd),
            suite_id,
            sizeof(suite_id)) != 0) {
        fprintf(stderr, "Failed to resolve suite id from %s\n", cli->suite_file);
        return 2;
    }

    printf("[Suite] Suite start: %s\n", suite_id);
    make_timestamp_id("run", run_id, sizeof(run_id));
    snprintf(suite_report_root, sizeof(suite_report_root), "%s/%s/%s", cli->output_dir, suite_id, run_id);
    snprintf(suite_log_root, sizeof(suite_log_root), "%s/%s/%s", cli->log_dir, suite_id, run_id);
    snprintf(summary_dir, sizeof(summary_dir), "%s/summary", suite_report_root);
    snprintf(resolved_tsv, sizeof(resolved_tsv), "%s/resolved_scenarios.tsv", summary_dir);
    snprintf(resolved_yaml, sizeof(resolved_yaml), "%s/suite_manifest_resolved.yaml", summary_dir);
    snprintf(results_tsv, sizeof(results_tsv), "%s/scenario_results.tsv", summary_dir);

    if (ensure_directory(summary_dir) != 0 || ensure_directory(suite_log_root) != 0) {
        fprintf(stderr, "Failed to create suite output directories.\n");
        return 2;
    }

    snprintf(cmd, sizeof(cmd),
             "python3 scripts/suites/resolve_suite.py --suite \"%s\" --resolved-tsv \"%s\" --resolved-yaml \"%s\" --default-users \"%s\"",
             cli->suite_file,
             resolved_tsv,
             resolved_yaml,
             cli->users_file);
    if (run_command_status(cmd) != 0) {
        fprintf(stderr, "Failed to resolve suite manifest: %s\n", cli->suite_file);
        return 2;
    }

    if (load_suite_scenarios(resolved_tsv, &scenarios, &scenario_count) != 0 || scenario_count <= 0) {
        fprintf(stderr, "Failed to load resolved scenarios from %s\n", resolved_tsv);
        free(scenarios);
        return 2;
    }
    if (write_suite_result_header(results_tsv) != 0) {
        fprintf(stderr, "Failed to create suite results manifest: %s\n", results_tsv);
        free(scenarios);
        return 2;
    }

    for (i = 0; i < scenario_count; i++) {
        ScenarioRunResult result;
        char scenario_report_dir[PATH_MAX];
        char scenario_log_dir[PATH_MAX];

        snprintf(scenario_report_dir, sizeof(scenario_report_dir), "%s/scenarios/%s", suite_report_root, scenarios[i].scenario_id);
        snprintf(scenario_log_dir, sizeof(scenario_log_dir), "%s/scenarios/%s", suite_log_root, scenarios[i].scenario_id);

        printf("\n=== Suite Scenario [%d/%d]: %s (%s) ===\n",
               i + 1, scenario_count, scenarios[i].scenario_id, scenarios[i].profile);
        printf("[Suite] Scenario start: %s (%s)\n", scenarios[i].scenario_id, scenarios[i].profile);

        memset(&result, 0, sizeof(result));
        result.exit_status = run_benchmark_scenario(cli, &scenarios[i], scenario_report_dir, scenario_log_dir, &result);
        if (result.exit_status == 0) {
            result.baseline_exit_status = run_perf_baseline_action(&scenarios[i], &result, run_id);
        }
        if (result.exit_status == 0 && scenarios[i].analyze_longrun) {
            result.longrun_exit_status = run_longrun_analysis(&scenarios[i], &result);
        }
        append_suite_result(results_tsv, run_id, &scenarios[i], &result);

        {
            const char *baseline_status = "SKIP";
            const char *longrun_status = "SKIP";
            const char *scenario_status = "PASS";

            if (result.exit_status == 0 && scenarios[i].baseline_enabled) {
                char gate_result_path[PATH_MAX];
                snprintf(gate_result_path, sizeof(gate_result_path), "%s/perf_gate/gate_result.json", result.report_dir);
                if (strcmp(scenarios[i].baseline_mode, "generate") == 0 && result.baseline_exit_status == 0) {
                    baseline_status = "UPDATED";
                } else if (strcmp(scenarios[i].baseline_mode, "compare") == 0) {
                    if (result.baseline_exit_status != 0) {
                        baseline_status = "FAIL";
                    } else if (file_contains_text(gate_result_path, "\"warned_metrics\": []")) {
                        baseline_status = "PASS";
                    } else if (file_contains_text(gate_result_path, "\"warned_metrics\": [")) {
                        baseline_status = "WARN";
                    } else {
                        baseline_status = "PASS";
                    }
                } else if (result.baseline_exit_status != 0) {
                    baseline_status = "FAIL";
                }
            }
            if (scenarios[i].analyze_longrun) {
                char longrun_summary_path[PATH_MAX];
                snprintf(longrun_summary_path, sizeof(longrun_summary_path), "%s/longrun_summary.json", result.report_dir);
                if (result.longrun_exit_status == 1) longrun_status = "FAIL";
                else if (file_contains_text(longrun_summary_path, "\"longrun_status\": \"WARN\"")) longrun_status = "WARN";
                else if (file_contains_text(longrun_summary_path, "\"longrun_status\": \"PASS\"")) longrun_status = "PASS";
            }
            if (result.exit_status != 0 || result.baseline_exit_status == 1 || result.baseline_exit_status == 2 || result.longrun_exit_status == 1) {
                scenario_status = "FAIL";
            } else if (strcmp(baseline_status, "WARN") == 0 || strcmp(longrun_status, "WARN") == 0) {
                scenario_status = "WARN";
            }

            printf("[Suite] Scenario final: %s\n", scenario_status);
            printf("[Suite] Baseline: %s\n", baseline_status);
            printf("[Suite] Longrun: %s\n", longrun_status);
        }

        if (result.exit_status == 2 || result.longrun_exit_status == 2 || result.baseline_exit_status == 2) overall_exit = 2;
        else if ((result.exit_status != 0 || result.longrun_exit_status == 1 || result.baseline_exit_status == 1) && overall_exit == 0) overall_exit = 1;

        if ((result.exit_status != 0 || result.longrun_exit_status != 0 || result.baseline_exit_status != 0) && !scenarios[i].continue_on_fail) {
            printf("[Suite] Stopping after scenario failure: %s\n", scenarios[i].scenario_id);
            break;
        }
        if (g_graceful_stop) break;
    }

    snprintf(cmd, sizeof(cmd),
             "python3 scripts/reporting/generate_suite_summary.py --suite-yaml \"%s\" --scenario-results \"%s\" --output-dir \"%s\"",
             resolved_yaml,
             results_tsv,
             summary_dir);
    if (run_command_status(cmd) != 0 && overall_exit == 0) {
        overall_exit = 2;
    }

    free(scenarios);
    return overall_exit;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [config.dat|testcase] [--config FILE] [--users FILE] [--op OP] [--threads N] [--object-size SPEC] [--output-dir DIR] [--log-dir DIR]\n",
            prog);
    fprintf(stderr, "       %s [--scenario-id ID]\n", prog);
    fprintf(stderr, "       %s [--suite FILE]\n", prog);
}

int main(int argc, char **argv) {
    CliOptions cli;
    char task_id[64];
    char report_dir[PATH_MAX];
    char log_dir[PATH_MAX];
    int ret;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_sigint);

    if (parse_cli_options(argc, argv, &cli) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (ensure_directory("upload_checkpoint") != 0) {
        fprintf(stderr, "Failed to create upload_checkpoint directory\n");
        return 1;
    }

    if (cli.suite_mode) {
        return run_suite_mode(&cli);
    }

    make_timestamp_id("task", task_id, sizeof(task_id));
    snprintf(report_dir, sizeof(report_dir), "%s/%s", cli.output_dir, task_id);
    snprintf(log_dir, sizeof(log_dir), "%s/%s", cli.log_dir, task_id);
    ret = run_benchmark_scenario(&cli, NULL, report_dir, log_dir, NULL);
    return ret;
}
