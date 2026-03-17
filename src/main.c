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

volatile sig_atomic_t g_graceful_stop = 0;

typedef struct {
    WorkerArgs *t_args;
    int thread_count;
    int interval_sec;
    volatile int stop_flag;
    char log_task_dir[PATH_MAX];
    int sample_count;
    int cpu_sample_count;
    int rss_sample_count;
    int wrote_samples;
    long long prev_total_requests;
    long long prev_streamed_bytes;
    double start_wall_s;
    double prev_wall_s;
    double prev_cpu_s;
    double cpu_sum_pct;
    double cpu_peak_pct;
    double rss_sum_mb;
    double rss_peak_mb;
    double peak_tps;
    double peak_bps;
} MonitorArgs;

typedef struct {
    char config_file[PATH_MAX];
    char users_file[PATH_MAX];
    char output_dir[PATH_MAX];
    char log_dir[PATH_MAX];
    char scenario_id[128];
    char object_size_spec[64];
    int op_set;
    int op;
    int threads_set;
    int threads;
    int object_size_set;
    int output_dir_set;
    int log_dir_set;
} CliOptions;

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
        fprintf(rt_fp, "%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%lld\n",
                elapsed_s,
                progress_pct >= 0.0 ? progress_pct : 0.0,
                cpu_pct,
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
        fprintf(rt_fp, "RunTime(s),Process(%%),CPU(%%),RSS(MB),Interval_TPS,Interval_BPS(Bytes/s),Cumul_TPS,Cumul_BPS(Bytes/s),Peak_TPS,Peak_BPS(Bytes/s),Success_Rate(%%),Total_Reqs\n");
        fflush(rt_fp);
    }

    m_args->start_wall_s = monotonic_seconds();
    m_args->prev_wall_s = m_args->start_wall_s;
    m_args->prev_cpu_s = process_cpu_seconds();

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
        {0, 0, 0, 0}
    };
    int opt;

    memset(cli, 0, sizeof(*cli));
    snprintf(cli->config_file, sizeof(cli->config_file), "config.dat");
    snprintf(cli->users_file, sizeof(cli->users_file), "users.dat");
    snprintf(cli->output_dir, sizeof(cli->output_dir), "reports");
    snprintf(cli->log_dir, sizeof(cli->log_dir), "logs");

    while ((opt = getopt_long(argc, argv, "c:u:o:t:s:S:O:L:", long_options, NULL)) != -1) {
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
            default:
                return -1;
        }
    }

    while (optind < argc) {
        int parsed_case = 0;
        if (!cli->op_set && parse_test_case_arg(argv[optind], &parsed_case) == 0) {
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
    }
    if (cli->object_size_set) {
        char errbuf[128] = {0};
        if (parse_object_size_spec(cli->object_size_spec, cfg, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "%s\n", errbuf);
            exit(1);
        }
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

    fprintf(fp, "task_id,start_time,config_file,users_file,op,users_loaded,total_threads,threads_per_user_effective,object_size_spec,actual_duration_s,total_requests,success_requests,failed_requests,success_rate_pct,avg_cpu_pct,peak_cpu_pct,avg_rss_mb,peak_rss_mb,final_tps,peak_tps,final_bps_bytes_per_sec,peak_bps_bytes_per_sec,avg_latency_ms,p99_latency_ms,avg_single_stream_bps,max_single_stream_bps,scenario_id,git_commit,host_os,host_arch\n");
    fprintf(fp, "%s,%s,%s,%s,%s,%d,%d,%.2f,%s,%.6f,%lld,%lld,%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%s,%s,%s,%s\n",
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

    snprintf(filepath, sizeof(filepath), "%s/brief.txt", cfg->report_task_dir);
    fp = fopen(filepath, "w");
    if (!fp) return;

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
    fprintf(fp, "  Total Threads:     %d\n", cfg->threads);
    fprintf(fp, "  Loaded Users:      %d\n", cfg->loaded_user_count);
    fprintf(fp, "  Avg Threads/User:  %.2f\n", threads_per_user_effective);
    fprintf(fp, "  RunSeconds:        %d %s\n", cfg->run_seconds, cfg->run_seconds > 0 ? "(Time Limited)" : "(No Limit)");
    fprintf(fp, "  Reqs/Thread:       %d\n", cfg->requests_per_thread);

    fprintf(fp, "[ObjectSettings]\n");
    fprintf(fp, "  ObjectSizeSpec:    %s\n", cfg->object_size_spec);
    if (cfg->is_dynamic_size) {
        fprintf(fp, "  ObjectSize:        %lld ~ %lld bytes (Dynamic)\n", cfg->object_size_min, cfg->object_size_max);
    } else {
        fprintf(fp, "  ObjectSize:        %lld bytes\n", cfg->object_size_max);
    }
    fprintf(fp, "  PartSize:          %lld bytes\n", cfg->part_size);
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
    fprintf(fp, "  Actual Duration:      %.4f s\n", summary->actual_duration_s);
    fprintf(fp, "  Success Rate:         %.4f %%\n", summary->success_rate_pct);
    fprintf(fp, "  Final TPS:            %.4f\n", summary->final_tps);
    fprintf(fp, "  Peak TPS:             %.4f\n", summary->peak_tps);
    fprintf(fp, "  Final BPS:            %.4f Bytes/s\n", summary->final_bps);
    fprintf(fp, "  Peak BPS:             %.4f Bytes/s\n", summary->peak_bps);
    fprintf(fp, "  Avg Latency:          %.4f ms\n", summary->avg_latency_ms);
    fprintf(fp, "  P99 Latency:          %.4f ms\n", summary->p99_latency_ms);
    fprintf(fp, "  Avg Single Stream:    %.4f Bytes/s\n", summary->avg_single_stream_bps);
    fprintf(fp, "  Max Single Stream:    %.4f Bytes/s\n", summary->max_single_stream_bps);
    fprintf(fp, "  Avg CPU:              %.4f %%\n", summary->avg_cpu_pct);
    fprintf(fp, "  Peak CPU:             %.4f %%\n", summary->peak_cpu_pct);
    fprintf(fp, "  Avg RSS:              %.4f MB\n", summary->avg_rss_mb);
    fprintf(fp, "  Peak RSS:             %.4f MB\n", summary->peak_rss_mb);
    fprintf(fp, "===========================================\n");

    fclose(fp);
    LOG_INFO("Execution report saved to: %s", filepath);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [config.dat|testcase] [--config FILE] [--users FILE] [--op OP] [--threads N] [--object-size SPEC] [--output-dir DIR] [--log-dir DIR]\n",
            prog);
    fprintf(stderr, "       %s [--scenario-id ID]\n", prog);
}

int main(int argc, char **argv) {
    Config cfg;
    CliOptions cli;
    pthread_t *tids = NULL;
    WorkerArgs *t_args = NULL;
    pthread_t monitor_tid;
    MonitorArgs m_args;
    BenchmarkSummary summary;
    struct timeval main_start_tv, main_end_tv;
    time_t now = time(NULL);
    struct tm t_res;
    struct tm *t = localtime_r(&now, &t_res);
    char task_id[64];
    double current_ms = 0.0;
    double stop_ms = 0.0;
    obs_status status;
    unsigned long long merged_hist[LATENCY_HIST_BUCKETS] = {0};
    long long total_latency_samples = 0;
    long long total_completed_bytes = 0;
    double single_stream_sum = 0.0;
    int single_stream_count = 0;
    int global_thread_idx = 0;
    int u;

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

    initialize_break_point_lock();
    memset(&cfg, 0, sizeof(cfg));
    memset(&summary, 0, sizeof(summary));
    memset(&m_args, 0, sizeof(m_args));

    if (t) {
        snprintf(task_id, sizeof(task_id), "task_%04d%02d%02d_%02d%02d%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        snprintf(task_id, sizeof(task_id), "task_UNKNOWN");
    }
    if (load_config(cli.config_file, &cfg) != 0) return 1;
    apply_cli_overrides(&cfg, &cli);

    snprintf(cfg.report_task_dir, sizeof(cfg.report_task_dir), "%s/%s", cli.output_dir, task_id);
    snprintf(cfg.log_task_dir, sizeof(cfg.log_task_dir), "%s/%s", cli.log_dir, task_id);
    if (ensure_directory(cli.output_dir) != 0 || ensure_directory(cfg.report_task_dir) != 0) {
        fprintf(stderr, "Failed to create report output directory: %s\n", cfg.report_task_dir);
        return 1;
    }
    if (ensure_directory(cli.log_dir) != 0 || ensure_directory(cfg.log_task_dir) != 0) {
        fprintf(stderr, "Failed to create log output directory: %s\n", cfg.log_task_dir);
        return 1;
    }

    log_init(cfg.log_level);
    LOG_INFO("--- OBS C SDK Benchmark Tool ---");
    LOG_INFO("Report Output Dir: %s", cfg.report_task_dir);
    LOG_INFO("Log Output Dir: %s", cfg.log_task_dir);

    if (cfg.test_case == TEST_CASE_MULTIPART && cfg.parts_for_each_upload_id <= 0) {
        LOG_ERROR("FATAL: TestCase 216 (Multipart Upload) requires 'PartsForEachUploadID' to be explicitly set in config.dat (valid range: 1~10000).");
        return 1;
    }

    if (cfg.is_temporary_token) {
        char cmd[256];
        LOG_INFO("IsTemporaryToken enabled. Fetching STS tokens for %d users...", cfg.target_user_count);
        snprintf(cmd, sizeof(cmd), "python3 scripts/sdk/generate_temp_ak_sk.py %d", cfg.target_user_count);
        if (system(cmd) != 0) {
            LOG_ERROR("FATAL: Failed to generate temporary credentials. (Command: %s)", cmd);
            return 1;
        }
        snprintf(cfg.users_file_path, sizeof(cfg.users_file_path), "%s", "temptoken.dat");
        if (load_users_file(cfg.users_file_path, &cfg, 1) < 0) return 1;
    } else {
        snprintf(cfg.users_file_path, sizeof(cfg.users_file_path), "%s", cli.users_file);
        if (load_users_file(cfg.users_file_path, &cfg, 0) < 0) return 1;
    }

    if (cfg.loaded_user_count <= 0) {
        LOG_ERROR("No users loaded from %s", cfg.users_file_path);
        return 1;
    }

    if (!cli.threads_set) {
        if (cfg.threads_per_user <= 0) cfg.threads_per_user = 1;
        cfg.threads = cfg.loaded_user_count * cfg.threads_per_user;
    } else {
        cfg.threads = cli.threads;
    }
    if (cfg.scenario_id[0] == '\0') {
        infer_scenario_id(&cfg, cfg.scenario_id, sizeof(cfg.scenario_id));
    }

    printf("[Config] Multi-User Mode: %d Users Loaded. Total Threads: %d\n", cfg.loaded_user_count, cfg.threads);
    printf("[Config] Operation: %s (%d)\n", test_case_to_name(cfg.test_case), cfg.test_case);
    printf("[Config] ObjectSize: %s\n", cfg.object_size_spec);
    if (cfg.enable_data_validation) printf("[Config] Data Validation: ENABLED\n");
    if (cfg.enable_detail_log) printf("[Config] Detail Request Log: ENABLED\n");

    status = obs_initialize(OBS_INIT_ALL);
    if (status != OBS_STATUS_OK) return -1;

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
        free(tids);
        free(t_args);
        obs_deinitialize();
        deinitialize_break_point_lock();
        return 1;
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

        if (cli.threads_set) {
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
    printf("Avg RSS:         %.4f MB\n", summary.avg_rss_mb);
    printf("Peak RSS:        %.4f MB\n", summary.peak_rss_mb);
    printf("Avg SingleFlow:  %.4f Bytes/s\n", summary.avg_single_stream_bps);
    printf("Max SingleFlow:  %.4f Bytes/s\n", summary.max_single_stream_bps);

    save_benchmark_report(&cfg, &summary);
    save_archive_csv(&cfg, &summary);

    free(tids);
    free(t_args);

    if (cfg.user_list) {
        free(cfg.user_list);
        cfg.user_list = NULL;
    }
    for (u = 0; u < cfg.range_count; u++) {
        if (cfg.range_options[u]) {
            free(cfg.range_options[u]);
            cfg.range_options[u] = NULL;
        }
    }

    obs_deinitialize();
    deinitialize_break_point_lock();
    return 0;
}
