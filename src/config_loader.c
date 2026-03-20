#include "bench.h"
#include <strings.h>
#include <ctype.h>
#include <errno.h>

#define CONFIG_LOADER_MAX_DEPTH 8

typedef enum {
    SIMPLE_SECTION_ROOT = 0,
    SIMPLE_SECTION_ADVANCED,
    SIMPLE_SECTION_SECURITY
} SimpleSection;
static void remember_unknown_config_key(Config *cfg, const char *key) {
    int i;

    if (!cfg || !key || key[0] == '\0') return;
    for (i = 0; i < cfg->unknown_config_key_count; i++) {
        if (strcmp(cfg->unknown_config_keys[i], key) == 0) return;
    }
    if (cfg->unknown_config_key_count < (int)(sizeof(cfg->unknown_config_keys) / sizeof(cfg->unknown_config_keys[0]))) {
        snprintf(cfg->unknown_config_keys[cfg->unknown_config_key_count],
                 sizeof(cfg->unknown_config_keys[cfg->unknown_config_key_count]),
                 "%s",
                 key);
        cfg->unknown_config_key_count++;
    }
}

static long long parse_size_token(const char *text, int *ok) {
    char *end = NULL;
    long long value;
    char unit_buf[8] = {0};
    size_t i = 0;

    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || value < 0) {
        *ok = 0;
        return 0;
    }

    while (*end && isspace((unsigned char)*end)) {
        end++;
    }

    while (*end && i < sizeof(unit_buf) - 1) {
        unit_buf[i++] = (char)toupper((unsigned char)*end);
        end++;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        *ok = 0;
        return 0;
    }

    *ok = 1;
    if (unit_buf[0] == '\0' || strcmp(unit_buf, "B") == 0) return value;
    if (strcmp(unit_buf, "KB") == 0) return value * 1000LL;
    if (strcmp(unit_buf, "MB") == 0) return value * 1000LL * 1000LL;
    if (strcmp(unit_buf, "GB") == 0) return value * 1000LL * 1000LL * 1000LL;
    if (strcmp(unit_buf, "TB") == 0) return value * 1000LL * 1000LL * 1000LL * 1000LL;

    *ok = 0;
    return 0;
}

static char* trim_both(char *s) {
    char *start;
    char *end;

    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') return s;

    start = s;
    end = start + strlen(start) - 1;
    while (end >= start && isspace((unsigned char)*end)) {
        *end = '\0';
        if (end == start) break;
        end--;
    }
    return start;
}

static int parse_mix_ops(const char *val, int *ops, int max_ops) {
    int count = 0;
    char *temp = strdup(val);
    if (!temp) return 0;
    char *token = strtok(temp, ",");
    while (token != NULL && count < max_ops) {
        char *clean_token = trim_both(token);
        if (strlen(clean_token) == 0) {
             token = strtok(NULL, ",");
             continue;
        }
        int op = atoi(clean_token);
        if (op == TEST_CASE_MIX) {
            token = strtok(NULL, ",");
            continue;
        }
        if (op > 0) ops[count++] = op;
        token = strtok(NULL, ",");
    }
    free(temp);
    return count;
}

static void reset_range_options(Config *cfg) {
    int i;
    if (!cfg) return;
    for (i = 0; i < MAX_RANGE_OPTIONS; i++) {
        if (cfg->range_options[i]) {
            free(cfg->range_options[i]);
            cfg->range_options[i] = NULL;
        }
    }
    cfg->range_count = 0;
}

static int parse_bool_text(const char *val) {
    return (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
            strcasecmp(val, "yes") == 0 || strcasecmp(val, "on") == 0);
}

static void strip_inline_comment(char *line) {
    int in_single = 0;
    int in_double = 0;
    char *cursor;

    if (!line) return;
    for (cursor = line; *cursor; cursor++) {
        if (*cursor == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*cursor == '"' && !in_single) {
            in_double = !in_double;
        } else if (*cursor == '#' && !in_single && !in_double) {
            if (cursor == line || isspace((unsigned char)cursor[-1])) {
                *cursor = '\0';
                break;
            }
        }
    }
}

static char *unquote_value(char *val) {
    size_t len;
    if (!val) return NULL;
    val = trim_both(val);
    len = strlen(val);
    if (len >= 2 && ((val[0] == '"' && val[len - 1] == '"') || (val[0] == '\'' && val[len - 1] == '\''))) {
        val[len - 1] = '\0';
        val++;
    }
    return trim_both(val);
}

static void resolve_path_relative_to_file(const char *base_file, const char *value, char *out, size_t out_size) {
    const char *slash;
    size_t dir_len;
    char joined[PATH_MAX];
    char resolved[PATH_MAX];

    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!value || value[0] == '\0') return;
    if (value[0] == '/' || !base_file || base_file[0] == '\0') {
        if (realpath(value, resolved)) snprintf(out, out_size, "%s", resolved);
        else snprintf(out, out_size, "%s", value);
        return;
    }

    slash = strrchr(base_file, '/');
    if (!slash) {
        if (realpath(value, resolved)) snprintf(out, out_size, "%s", resolved);
        else snprintf(out, out_size, "%s", value);
        return;
    }
    dir_len = (size_t)(slash - base_file);
    if (dir_len == 0) {
        snprintf(joined, sizeof(joined), "/%s", value);
        if (realpath(joined, resolved)) snprintf(out, out_size, "%s", resolved);
        else snprintf(out, out_size, "%s", joined);
        return;
    }
    snprintf(joined, sizeof(joined), "%.*s/%s", (int)dir_len, base_file, value);
    if (realpath(joined, resolved)) snprintf(out, out_size, "%s", resolved);
    else snprintf(out, out_size, "%s", joined);
}

int parse_object_size_spec(const char *spec, Config *cfg, char *errbuf, size_t errbuf_size) {
    char temp[128];
    char spec_copy[128];
    char *range_sep = NULL;
    int ok = 0;
    long long min_size = 0;
    long long max_size = 0;

    if (!spec || !cfg) {
        if (errbuf && errbuf_size > 0) snprintf(errbuf, errbuf_size, "ObjectSize spec is missing");
        return -1;
    }

    snprintf(temp, sizeof(temp), "%s", spec);
    snprintf(spec_copy, sizeof(spec_copy), "%s", spec);
    range_sep = strchr(temp, '~');

    if (range_sep) {
        *range_sep = '\0';
        min_size = parse_size_token(trim_both(temp), &ok);
        if (!ok) {
            if (errbuf && errbuf_size > 0) snprintf(errbuf, errbuf_size, "Invalid ObjectSize lower bound: %s", spec);
            return -1;
        }
        max_size = parse_size_token(trim_both(range_sep + 1), &ok);
        if (!ok) {
            if (errbuf && errbuf_size > 0) snprintf(errbuf, errbuf_size, "Invalid ObjectSize upper bound: %s", spec);
            return -1;
        }
        if (min_size > max_size) {
            if (errbuf && errbuf_size > 0) snprintf(errbuf, errbuf_size, "Invalid ObjectSize range: %s", spec);
            return -1;
        }
        cfg->object_size_min = min_size;
        cfg->object_size_max = max_size;
        cfg->is_dynamic_size = 1;
        cfg->object_size = max_size;
    } else {
        max_size = parse_size_token(trim_both(temp), &ok);
        if (!ok) {
            if (errbuf && errbuf_size > 0) snprintf(errbuf, errbuf_size, "Invalid ObjectSize value: %s", spec);
            return -1;
        }
        cfg->object_size_min = max_size;
        cfg->object_size_max = max_size;
        cfg->is_dynamic_size = 0;
        cfg->object_size = max_size;
    }

    snprintf(cfg->object_size_spec, sizeof(cfg->object_size_spec), "%s", trim_both(spec_copy));
    return 0;
}

int parse_test_case_arg(const char *value, int *out_test_case) {
    if (!value || !out_test_case) return -1;

    if (isdigit((unsigned char)value[0])) {
        int test_case = atoi(value);
        switch (test_case) {
            case TEST_CASE_PUT:
            case TEST_CASE_GET:
            case TEST_CASE_DELETE:
            case TEST_CASE_MULTIPART:
            case TEST_CASE_RESUMABLE:
            case TEST_CASE_MIX:
                *out_test_case = test_case;
                return 0;
            default:
                return -1;
        }
    }

    if (strcasecmp(value, "upload") == 0 || strcasecmp(value, "put") == 0) {
        *out_test_case = TEST_CASE_PUT;
        return 0;
    }
    if (strcasecmp(value, "download") == 0 || strcasecmp(value, "get") == 0) {
        *out_test_case = TEST_CASE_GET;
        return 0;
    }
    if (strcasecmp(value, "delete") == 0 || strcasecmp(value, "del") == 0) {
        *out_test_case = TEST_CASE_DELETE;
        return 0;
    }
    if (strcasecmp(value, "multipart") == 0 || strcasecmp(value, "mpu") == 0) {
        *out_test_case = TEST_CASE_MULTIPART;
        return 0;
    }
    if (strcasecmp(value, "resumable") == 0 || strcasecmp(value, "uploadfile") == 0) {
        *out_test_case = TEST_CASE_RESUMABLE;
        return 0;
    }
    if (strcasecmp(value, "mix") == 0) {
        *out_test_case = TEST_CASE_MIX;
        return 0;
    }
    return -1;
}

const char *test_case_to_name(int test_case) {
    switch (test_case) {
        case TEST_CASE_PUT: return "upload";
        case TEST_CASE_GET: return "download";
        case TEST_CASE_DELETE: return "delete";
        case TEST_CASE_MULTIPART: return "multipart";
        case TEST_CASE_RESUMABLE: return "resumable";
        case TEST_CASE_MIX: return "mix";
        default: return "unknown";
    }
}

int load_users_file(const char *filename, Config *cfg, int is_temp_mode) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("[Config Error] Cannot open users file: %s.\n", filename);
        return -1;
    }
    if (cfg->target_user_count <= 0) cfg->target_user_count = 1;

    cfg->user_list = (UserCredential *)malloc(sizeof(UserCredential) * cfg->target_user_count);
    memset(cfg->user_list, 0, sizeof(UserCredential) * cfg->target_user_count);

    char line[4096];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < cfg->target_user_count) {
        char *clean_line = trim_both(line);
        if (clean_line[0] == '#' || strlen(clean_line) == 0) continue;

        char *token = strtok(clean_line, ",");
        if(token) strcpy(cfg->user_list[count].username, trim_both(token));
        else continue;

        token = strtok(NULL, ",");
        if(token) strcpy(cfg->user_list[count].ak, trim_both(token));
        else continue;

        token = strtok(NULL, ",");
        if(token) strcpy(cfg->user_list[count].sk, trim_both(token));
        else continue;

        if (is_temp_mode) {
            token = strtok(NULL, ",");
            if(token) strcpy(cfg->user_list[count].security_token, trim_both(token));

            token = strtok(NULL, ",");
            if(token) strcpy(cfg->user_list[count].original_ak, trim_both(token));
            else memset(cfg->user_list[count].original_ak, 0, sizeof(cfg->user_list[count].original_ak));
        } else {
            memset(cfg->user_list[count].security_token, 0, sizeof(cfg->user_list[count].security_token));
            strcpy(cfg->user_list[count].original_ak, cfg->user_list[count].ak);
        }

        count++;
    }
    fclose(fp);
    cfg->loaded_user_count = count;
    return count;
}

static void init_config_defaults(Config *cfg) {
    int i;

    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    cfg->connect_timeout_sec = 10;
    cfg->request_timeout_sec = 30;
    strcpy(cfg->protocol, "https");
    cfg->keep_alive = 1;
    cfg->part_size = 5 * 1024 * 1024;
    cfg->parts_for_each_upload_id = 0;
    cfg->log_level = LOG_INFO;
    cfg->obj_name_pattern_hash = 0;
    cfg->enable_checkpoint = 1;
    cfg->upload_file_path[0] = '\0';
    cfg->requests_per_thread = 1;
    cfg->mix_op_count = 0;
    cfg->mix_loop_count = 0;
    cfg->use_mix_mode = 0;
    cfg->run_seconds = 0;
    cfg->allow_open_ended_run = 0;
    cfg->target_user_count = 0;
    cfg->threads_per_user = 1;
    cfg->bucket_name_fixed[0] = '\0';
    cfg->bucket_name_prefix[0] = '\0';
    cfg->is_temporary_token = 0;
    cfg->resumable_task_num = 5;
    cfg->gm_auth_mode[0] = '\0';
    cfg->server_cert_path[0] = '\0';
    cfg->client_sign_cert_path[0] = '\0';
    cfg->client_sign_key_path[0] = '\0';
    cfg->client_sign_key_password[0] = '\0';
    cfg->client_enc_cert_path[0] = '\0';
    cfg->client_enc_key_path[0] = '\0';
    cfg->enable_data_validation = 0;
    cfg->enable_detail_log = 0;
    cfg->analyze_longrun = 0;
    cfg->gate_longrun = 0;
    cfg->config_file_path[0] = '\0';
    cfg->users_file_path[0] = '\0';
    cfg->effective_config_path[0] = '\0';
    cfg->unknown_config_key_count = 0;
    cfg->run_seconds_source = CONFIG_SOURCE_DEFAULT;
    cfg->requests_per_thread_source = CONFIG_SOURCE_DEFAULT;
    cfg->allow_open_ended_run_source = CONFIG_SOURCE_DEFAULT;
    cfg->range_source = CONFIG_SOURCE_DEFAULT;
    cfg->part_size_source = CONFIG_SOURCE_DEFAULT;
    cfg->parts_for_each_upload_id_source = CONFIG_SOURCE_DEFAULT;
    cfg->upload_file_path_source = CONFIG_SOURCE_DEFAULT;
    cfg->enable_checkpoint_source = CONFIG_SOURCE_DEFAULT;
    cfg->enable_detail_log_source = CONFIG_SOURCE_DEFAULT;
    cfg->gm_auth_mode_source = CONFIG_SOURCE_DEFAULT;
    cfg->server_cert_path_source = CONFIG_SOURCE_DEFAULT;
    cfg->client_sign_cert_path_source = CONFIG_SOURCE_DEFAULT;
    cfg->client_sign_key_path_source = CONFIG_SOURCE_DEFAULT;
    cfg->client_sign_key_password_source = CONFIG_SOURCE_DEFAULT;
    cfg->client_enc_cert_path_source = CONFIG_SOURCE_DEFAULT;
    cfg->client_enc_key_path_source = CONFIG_SOURCE_DEFAULT;
    cfg->object_size_source = CONFIG_SOURCE_DEFAULT;
    cfg->threads_source = CONFIG_SOURCE_DEFAULT;
    
    cfg->object_size_min = cfg->object_size_max = 1024;
    cfg->is_dynamic_size = 0;
    cfg->object_size = 1024;
    snprintf(cfg->object_size_spec, sizeof(cfg->object_size_spec), "1024");
    cfg->range_count = 0;
    for (i = 0; i < MAX_RANGE_OPTIONS; i++) cfg->range_options[i] = NULL;
}

static int finalize_config(Config *cfg, const char *source_name, int allow_default_user_count) {
    if (!cfg) return -1;

    if (cfg->part_size <= 0) cfg->part_size = 5 * 1024 * 1024;
    if (cfg->target_user_count <= 0 && allow_default_user_count) cfg->target_user_count = 1;
    
    if (cfg->part_size <= 0) cfg->part_size = 5 * 1024 * 1024; 
    if (cfg->target_user_count <= 0) {
        printf("[Config Error] 'Users' must be greater than 0. Source: %s\n", source_name ? source_name : "unknown");
        return -1;
    }

    if (cfg->test_case == TEST_CASE_MIX) {
        if (cfg->mix_op_count > 0) cfg->use_mix_mode = 1;
    } else {
        cfg->use_mix_mode = 0;
    }

    if (strlen(cfg->gm_auth_mode) > 0) {
        if (strcasecmp(cfg->protocol, "https") != 0) {
            printf("[Config Error] Protocol MUST be 'https' when GmAuthMode is configured.\n");
            return -1;
        }

        if (strcmp(cfg->gm_auth_mode, "GM_Mutual") == 0) {
            if (strlen(cfg->server_cert_path) == 0 || strlen(cfg->client_sign_cert_path) == 0 ||
                strlen(cfg->client_sign_key_path) == 0 || strlen(cfg->client_enc_cert_path) == 0 ||
                strlen(cfg->client_enc_key_path) == 0) {
                printf("[Config Error] Required certificates/keys missing for GM_Mutual.\n");
                return -1;
            }
        } else if (strcmp(cfg->gm_auth_mode, "GM_Oneway") == 0) {
            if (strlen(cfg->server_cert_path) == 0) {
                printf("[Config Error] ServerCertPath missing for GM_Oneway.\n");
                return -1;
            }
        } else if (strcmp(cfg->gm_auth_mode, "Inter_Mutual") == 0) {
            if (strlen(cfg->server_cert_path) == 0 || strlen(cfg->client_sign_cert_path) == 0 ||
                strlen(cfg->client_sign_key_path) == 0) {
                printf("[Config Error] Required certificates/keys missing for Inter_Mutual.\n");
                return -1;
            }
        } else if (strcmp(cfg->gm_auth_mode, "Inter_Oneway") == 0) {
            if (strlen(cfg->server_cert_path) == 0) {
                printf("[Config Error] ServerCertPath missing for Inter_Oneway.\n");
                return -1;
            }
        }
    }

    return 0;
}

static int apply_config_entry(Config *cfg, const char *key, const char *val, const char *source_name, int is_simple_yaml) {
    if (!cfg || !key || !val) return -1;

    if (strcmp(key, "Endpoint") == 0) strcpy(cfg->endpoint, val);
    else if (strcmp(key, "Protocol") == 0) strcpy(cfg->protocol, val);
    else if (strcmp(key, "KeepAlive") == 0) cfg->keep_alive = parse_bool_text(val);
    else if (strcmp(key, "ConnectTimeoutSec") == 0) {
        if (strlen(val) > 0) {
            cfg->connect_timeout_sec = atoi(val);
            if (cfg->connect_timeout_sec <= 0) {
                printf("[Config Error] 'ConnectTimeoutSec' must be > 0. Invalid value: %s\n", val);
                return -1;
            }
        }
    }
    else if (strcmp(key, "RequestTimeoutSec") == 0) {
        if (strlen(val) > 0) {
            cfg->request_timeout_sec = atoi(val);
            if (cfg->request_timeout_sec <= 0) {
                printf("[Config Error] 'RequestTimeoutSec' must be > 0. Invalid value: %s\n", val);
                return -1;
            }
        }
    }
    else if (strcmp(key, "LogLevel") == 0) cfg->log_level = log_level_from_string(val);
    else if (strcmp(key, "ObjNamePatternHash") == 0) cfg->obj_name_pattern_hash = parse_bool_text(val);
    else if (strcmp(key, "EnableCheckpoint") == 0) {
        cfg->enable_checkpoint = parse_bool_text(val);
        cfg->enable_checkpoint_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "UploadFilePath") == 0) {
        strcpy(cfg->upload_file_path, val);
        cfg->upload_file_path_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "BucketNamePrefix") == 0) strcpy(cfg->bucket_name_prefix, val);
    else if (strcmp(key, "BucketNameFixed") == 0) strcpy(cfg->bucket_name_fixed, val);
    else if (strcmp(key, "IsTemporaryToken") == 0) cfg->is_temporary_token = parse_bool_text(val);
    else if (strcmp(key, "Users") == 0) cfg->target_user_count = atoi(val);
    else if (strcmp(key, "ThreadsPerUser") == 0) cfg->threads_per_user = atoi(val);
    else if (strcmp(key, "Threads") == 0) {
        cfg->threads = atoi(val);
        cfg->threads_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "RequestsPerThread") == 0) {
        cfg->requests_per_thread = atoi(val);
        cfg->requests_per_thread_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "TestCase") == 0) {
        if (is_simple_yaml) {
            if (parse_test_case_arg(val, &cfg->test_case) != 0) {
                printf("[Config Error] Invalid operation/TestCase value in %s: %s\n", source_name ? source_name : "simple yaml", val);
                return -1;
            }
        } else {
            cfg->test_case = atoi(val);
        }
    }
    else if (strcmp(key, "ObjectSize") == 0) {
        char errbuf[128] = {0};
        if (parse_object_size_spec(val, cfg, errbuf, sizeof(errbuf)) != 0) {
            printf("[Config Error] %s\n", errbuf);
            return -1;
        }
        cfg->object_size_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "Range") == 0) {
        char *temp;
        reset_range_options(cfg);
        temp = strdup(val);
        if (temp) {
            char *token = strtok(temp, ";");
            int idx = 0;
            while (token != NULL && idx < MAX_RANGE_OPTIONS) {
                char *clean_token = trim_both(token);
                if (strlen(clean_token) > 0) {
                    cfg->range_options[idx] = strdup(clean_token);
                    idx++;
                }
                token = strtok(NULL, ";");
            }
            cfg->range_count = idx;
            free(temp);
        }
        cfg->range_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "PartSize") == 0) {
        cfg->part_size = atoll(val);
        cfg->part_size_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "PartsForEachUploadID") == 0) {
        cfg->parts_for_each_upload_id = atoi(val);
        cfg->parts_for_each_upload_id_source = CONFIG_SOURCE_CONFIG;
        if (cfg->parts_for_each_upload_id < 0) {
            cfg->parts_for_each_upload_id = 0;
        } else if (cfg->parts_for_each_upload_id > 10000) {
            printf("[WARN] PartsForEachUploadID (%d) exceeds OBS max limit. Capped to 10000.\n", cfg->parts_for_each_upload_id);
            cfg->parts_for_each_upload_id = 10000;
        }
    }
    else if (strcmp(key, "KeyPrefix") == 0) strcpy(cfg->key_prefix, val);
    else if (strcmp(key, "MixOperation") == 0) cfg->mix_op_count = parse_mix_ops(val, cfg->mix_ops, MAX_MIX_OPS);
    else if (strcmp(key, "MixLoopCount") == 0) cfg->mix_loop_count = atoll(val);
    else if (strcmp(key, "RunSeconds") == 0) {
        cfg->run_seconds = atoi(val);
        cfg->run_seconds_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "AllowOpenEndedRun") == 0) {
        cfg->allow_open_ended_run = parse_bool_text(val);
        cfg->allow_open_ended_run_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "GmAuthMode") == 0) {
        strcpy(cfg->gm_auth_mode, val);
        cfg->gm_auth_mode_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "ServerCertPath") == 0) {
        strcpy(cfg->server_cert_path, val);
        cfg->server_cert_path_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "ClientSignCertPath") == 0) {
        strcpy(cfg->client_sign_cert_path, val);
        cfg->client_sign_cert_path_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "ClientSignKeyPath") == 0) {
        strcpy(cfg->client_sign_key_path, val);
        cfg->client_sign_key_path_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "ClientSignKeyPassword") == 0) {
        strcpy(cfg->client_sign_key_password, val);
        cfg->client_sign_key_password_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "ClientEncCertPath") == 0) {
        strcpy(cfg->client_enc_cert_path, val);
        cfg->client_enc_cert_path_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "ClientEncKeyPath") == 0) {
        strcpy(cfg->client_enc_key_path, val);
        cfg->client_enc_key_path_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "EnableDataValidation") == 0) cfg->enable_data_validation = parse_bool_text(val);
    else if (strcmp(key, "EnableDetailLog") == 0) {
        cfg->enable_detail_log = parse_bool_text(val);
        cfg->enable_detail_log_source = CONFIG_SOURCE_CONFIG;
    }
    else if (strcmp(key, "ResumableTaskNum") == 0) cfg->resumable_task_num = atoi(val);
    else if (strcmp(key, "AnalyzeLongrun") == 0) cfg->analyze_longrun = parse_bool_text(val);
    else if (strcmp(key, "GateLongrun") == 0) cfg->gate_longrun = parse_bool_text(val);
    else if (strcmp(key, "ScenarioId") == 0) snprintf(cfg->scenario_id, sizeof(cfg->scenario_id), "%s", val);
    else if (strcmp(key, "UsersFile") == 0) snprintf(cfg->users_file_path, sizeof(cfg->users_file_path), "%s", val);
    else if (strcmp(key, "Profile") == 0) {
        /* handled before field application */
    }
    else {
        if (is_simple_yaml) {
            printf("[Config Error] Unsupported config field '%s' in %s\n", key, source_name ? source_name : "config");
            return -1;
        }
    }

    return 0;
}

static int normalize_simple_yaml_key(const char *yaml_key, char *normalized, size_t normalized_size) {
    struct KeyAlias {
        const char *yaml_name;
        const char *config_name;
    };
    static const struct KeyAlias aliases[] = {
        {"endpoint", "Endpoint"},
        {"protocol", "Protocol"},
        {"keep_alive", "KeepAlive"},
        {"connect_timeout_sec", "ConnectTimeoutSec"},
        {"request_timeout_sec", "RequestTimeoutSec"},
        {"log_level", "LogLevel"},
        {"obj_name_pattern_hash", "ObjNamePatternHash"},
        {"enable_checkpoint", "EnableCheckpoint"},
        {"upload_file_path", "UploadFilePath"},
        {"bucket_name_prefix", "BucketNamePrefix"},
        {"bucket_name_fixed", "BucketNameFixed"},
        {"is_temporary_token", "IsTemporaryToken"},
        {"users", "UsersFile"},
        {"users_file", "UsersFile"},
        {"users_count", "Users"},
        {"threads_per_user", "ThreadsPerUser"},
        {"threads", "Threads"},
        {"requests_per_thread", "RequestsPerThread"},
        {"op", "TestCase"},
        {"test_case", "TestCase"},
        {"object_size", "ObjectSize"},
        {"range", "Range"},
        {"part_size", "PartSize"},
        {"parts_for_each_upload_id", "PartsForEachUploadID"},
        {"key_prefix", "KeyPrefix"},
        {"mix_operation", "MixOperation"},
        {"mix_loop_count", "MixLoopCount"},
        {"run_seconds", "RunSeconds"},
        {"allow_open_ended_run", "AllowOpenEndedRun"},
        {"gm_auth_mode", "GmAuthMode"},
        {"server_cert_path", "ServerCertPath"},
        {"client_sign_cert_path", "ClientSignCertPath"},
        {"client_sign_key_path", "ClientSignKeyPath"},
        {"client_sign_key_password", "ClientSignKeyPassword"},
        {"client_enc_cert_path", "ClientEncCertPath"},
        {"client_enc_key_path", "ClientEncKeyPath"},
        {"enable_data_validation", "EnableDataValidation"},
        {"enable_detail_log", "EnableDetailLog"},
        {"resumable_task_num", "ResumableTaskNum"},
        {"analyze_longrun", "AnalyzeLongrun"},
        {"gate_longrun", "GateLongrun"},
        {"scenario_id", "ScenarioId"},
        {"profile", "Profile"}
    };
    size_t i;

    if (!yaml_key || !normalized || normalized_size == 0) return -1;
    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(yaml_key, aliases[i].yaml_name) == 0) {
            snprintf(normalized, normalized_size, "%s", aliases[i].config_name);
            return 0;
        }
    }
    return -1;
}

static int load_simple_yaml_config_internal(const char *filename, Config *cfg, int depth);
static int load_config_internal(const char *filename, Config *cfg, int depth);

static int load_simple_yaml_config_internal(const char *filename, Config *cfg, int depth) {
    FILE *fp;
    char line[1024];
    int line_no = 0;
    SimpleSection section = SIMPLE_SECTION_ROOT;
    int profile_loaded = 0;
    int saw_non_profile_key = 0;

    if (depth > CONFIG_LOADER_MAX_DEPTH) {
        printf("[Config Error] Config include/profile nesting is too deep: %s\n", filename);
        return -1;
    }

    fp = fopen(filename, "r");
    if (!fp) {
        printf("Error: Cannot open simple config file: %s\n", filename);
        return -1;
    }

    init_config_defaults(cfg);

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed;
        char *colon;
        char key_buf[128];
        char normalized_key[128];
        char *value;
        int indent = 0;
        char resolved_path[PATH_MAX];

        line_no++;
        while (line[indent] == ' ' || line[indent] == '\t') indent++;
        strip_inline_comment(line);
        trimmed = trim_both(line);
        if (!trimmed || trimmed[0] == '\0') continue;
        if (trimmed[0] == '-') {
            printf("[Config Error] Lists are not supported in simple scenario config (%s:%d).\n", filename, line_no);
            fclose(fp);
            return -1;
        }

        colon = strchr(trimmed, ':');
        if (!colon) {
            printf("[Config Error] Invalid YAML line in %s:%d -> %s\n", filename, line_no, trimmed);
            fclose(fp);
            return -1;
        }
        *colon = '\0';
        snprintf(key_buf, sizeof(key_buf), "%s", trim_both(trimmed));
        value = unquote_value(colon + 1);

        if (indent == 0 && value[0] == '\0') {
            if (strcmp(key_buf, "advanced") == 0) section = SIMPLE_SECTION_ADVANCED;
            else if (strcmp(key_buf, "security") == 0) section = SIMPLE_SECTION_SECURITY;
            else {
                printf("[Config Error] Unsupported section '%s' in %s:%d\n", key_buf, filename, line_no);
                fclose(fp);
                return -1;
            }
            continue;
        }

        if (indent > 0 && !(section == SIMPLE_SECTION_ADVANCED || section == SIMPLE_SECTION_SECURITY)) {
            printf("[Config Error] Nested keys are only supported under advanced:/security: in %s:%d\n", filename, line_no);
            fclose(fp);
            return -1;
        }
        if (indent == 0) section = SIMPLE_SECTION_ROOT;

        if (normalize_simple_yaml_key(key_buf, normalized_key, sizeof(normalized_key)) != 0) {
            printf("[Config Error] Unsupported simple config field '%s' in %s:%d\n", key_buf, filename, line_no);
            fclose(fp);
            return -1;
        }

        if (strcmp(normalized_key, "Profile") == 0) {
            if (profile_loaded) {
                printf("[Config Error] Only one profile is supported in %s\n", filename);
                fclose(fp);
                return -1;
            }
            if (saw_non_profile_key) {
                printf("[Config Error] profile must be declared before other simple config fields in %s\n", filename);
                fclose(fp);
                return -1;
            }
            resolve_path_relative_to_file(filename, value, resolved_path, sizeof(resolved_path));
            if (load_config_internal(resolved_path, cfg, depth + 1) != 0) {
                fclose(fp);
                return -1;
            }
            profile_loaded = 1;
            snprintf(cfg->config_file_path, sizeof(cfg->config_file_path), "%s", filename);
            continue;
        }

        saw_non_profile_key = 1;

        if (strcmp(normalized_key, "UsersFile") == 0 ||
            strcmp(normalized_key, "UploadFilePath") == 0 ||
            strcmp(normalized_key, "ServerCertPath") == 0 ||
            strcmp(normalized_key, "ClientSignCertPath") == 0 ||
            strcmp(normalized_key, "ClientSignKeyPath") == 0 ||
            strcmp(normalized_key, "ClientEncCertPath") == 0 ||
            strcmp(normalized_key, "ClientEncKeyPath") == 0) {
            resolve_path_relative_to_file(filename, value, resolved_path, sizeof(resolved_path));
            value = resolved_path;
        }

        if (apply_config_entry(cfg, normalized_key, value, filename, 1) != 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    if (finalize_config(cfg, filename, 1) != 0) return -1;
    snprintf(cfg->config_file_path, sizeof(cfg->config_file_path), "%s", filename);
    if (cfg->unknown_config_key_count > 0) {
        int i;
        printf("[Config Warning] Unknown keys in %s:", filename);
        for (i = 0; i < cfg->unknown_config_key_count; i++) {
            printf("%s%s", i == 0 ? " " : ", ", cfg->unknown_config_keys[i]);
        }
        printf("\n");
    }
    return 0;
}

static int load_legacy_config_internal(const char *filename, Config *cfg) {
    FILE *fp = fopen(filename, "r");
    char line[512];

    if (!fp) {
        printf("Error: Cannot open config file: %s\n", filename);
        return -1;
    }

    init_config_defaults(cfg);

    while (fgets(line, sizeof(line), fp)) {
        char *clean_line;
        char *eq;
        char *key;
        char *val;

        clean_line = trim_both(line);
        if (clean_line[0] == '#' || clean_line[0] == '[' || strlen(clean_line) == 0) continue;

        eq = strchr(clean_line, '=');
        if (!eq) continue;
        *eq = '\0';

        key = trim_both(clean_line);
        val = trim_both(eq + 1);

        if (apply_config_entry(cfg, key, val, filename, 0) != 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    if (finalize_config(cfg, filename, 0) != 0) return -1;
    snprintf(cfg->config_file_path, sizeof(cfg->config_file_path), "%s", filename);
    return 0;
}

static int has_yaml_extension(const char *filename) {
    const char *dot = filename ? strrchr(filename, '.') : NULL;
    if (!dot) return 0;
    return (strcasecmp(dot, ".yaml") == 0 || strcasecmp(dot, ".yml") == 0);
}

static int load_config_internal(const char *filename, Config *cfg, int depth) {
    if (has_yaml_extension(filename)) {
        return load_simple_yaml_config_internal(filename, cfg, depth);
    }
    return load_legacy_config_internal(filename, cfg);
}

int load_config(const char *filename, Config *cfg) {
    return load_config_internal(filename, cfg, 0);
}
