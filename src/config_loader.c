#include "bench.h"
#include <strings.h>
#include <ctype.h>
#include <errno.h>

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
    if (!s) return NULL;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
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

int load_config(const char *filename, Config *cfg) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("Error: Cannot open config file: %s\n", filename);
        return -1;
    }
    
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
    cfg->target_user_count = 0;
    cfg->threads_per_user = 1;
    cfg->bucket_name_fixed[0] = '\0';
    cfg->bucket_name_prefix[0] = '\0';
    cfg->is_temporary_token = 0;  
    cfg->resumable_task_num = 5; 
    
    // 初始化安全认证路径
    cfg->gm_auth_mode[0] = '\0';
    cfg->server_cert_path[0] = '\0';
    cfg->client_sign_cert_path[0] = '\0';
    cfg->client_sign_key_path[0] = '\0';
    cfg->client_sign_key_password[0] = '\0';
    cfg->client_enc_cert_path[0] = '\0';
    cfg->client_enc_key_path[0] = '\0';
    
    cfg->enable_data_validation = 0;
    cfg->enable_detail_log = 0;
    cfg->config_file_path[0] = '\0';
    cfg->users_file_path[0] = '\0';
    
    cfg->object_size_min = cfg->object_size_max = 1024;
    cfg->is_dynamic_size = 0;
    cfg->object_size = 1024;
    snprintf(cfg->object_size_spec, sizeof(cfg->object_size_spec), "1024");
    
    cfg->range_count = 0;
    for(int i=0; i<MAX_RANGE_OPTIONS; i++) cfg->range_options[i] = NULL;

    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        char *clean_line = trim_both(line);
        if (clean_line[0] == '#' || clean_line[0] == '[' || strlen(clean_line) == 0) continue;

        char *eq = strchr(clean_line, '=');
        if (!eq) continue;
        *eq = '\0';
        
        char *key = trim_both(clean_line);
        char *val = trim_both(eq + 1);

        if (strcmp(key, "Endpoint") == 0) strcpy(cfg->endpoint, val);
        else if (strcmp(key, "Protocol") == 0) strcpy(cfg->protocol, val);
        else if (strcmp(key, "KeepAlive") == 0) cfg->keep_alive = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        // [新增校验]: 对连接超时的配置值进行合法性检查
        else if (strcmp(key, "ConnectTimeoutSec") == 0) {
            if (strlen(val) > 0) {
                cfg->connect_timeout_sec = atoi(val);
                if (cfg->connect_timeout_sec <= 0) {
                    printf("[Config Error] 'ConnectTimeoutSec' must be > 0. Invalid value: %s\n", val);
                    fclose(fp); return -1;
                }
            }
        }
        else if (strcmp(key, "RequestTimeoutSec") == 0) {
            if (strlen(val) > 0) {
                cfg->request_timeout_sec = atoi(val);
                if (cfg->request_timeout_sec <= 0) {
                    printf("[Config Error] 'RequestTimeoutSec' must be > 0. Invalid value: %s\n", val);
                    fclose(fp); return -1;
                }
            }
        }
        else if (strcmp(key, "LogLevel") == 0) cfg->log_level = log_level_from_string(val);
        else if (strcmp(key, "ObjNamePatternHash") == 0) cfg->obj_name_pattern_hash = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "EnableCheckpoint") == 0) cfg->enable_checkpoint = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "UploadFilePath") == 0) strcpy(cfg->upload_file_path, val);
        else if (strcmp(key, "BucketNamePrefix") == 0) strcpy(cfg->bucket_name_prefix, val);
        else if (strcmp(key, "BucketNameFixed") == 0) strcpy(cfg->bucket_name_fixed, val);
        else if (strcmp(key, "IsTemporaryToken") == 0) cfg->is_temporary_token = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "Users") == 0) cfg->target_user_count = atoi(val);
        else if (strcmp(key, "ThreadsPerUser") == 0) cfg->threads_per_user = atoi(val);
        else if (strcmp(key, "RequestsPerThread") == 0) cfg->requests_per_thread = atoi(val);
        else if (strcmp(key, "TestCase") == 0) cfg->test_case = atoi(val);
        
        else if (strcmp(key, "ObjectSize") == 0) {
            char errbuf[128] = {0};
            if (parse_object_size_spec(val, cfg, errbuf, sizeof(errbuf)) != 0) {
                printf("[Config Error] %s\n", errbuf);
                fclose(fp); return -1;
            }
        }
        else if (strcmp(key, "Range") == 0) {
            char *temp = strdup(val);
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
        }
        else if (strcmp(key, "PartSize") == 0) cfg->part_size = atoll(val);
        else if (strcmp(key, "PartsForEachUploadID") == 0) {
            cfg->parts_for_each_upload_id = atoi(val);
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
        else if (strcmp(key, "RunSeconds") == 0) cfg->run_seconds = atoi(val);
        
        // ------------------
        // 安全配置映射
        // ------------------
        else if (strcmp(key, "GmAuthMode") == 0) strcpy(cfg->gm_auth_mode, val);
        else if (strcmp(key, "ServerCertPath") == 0) strcpy(cfg->server_cert_path, val);
        else if (strcmp(key, "ClientSignCertPath") == 0) strcpy(cfg->client_sign_cert_path, val);
        else if (strcmp(key, "ClientSignKeyPath") == 0) strcpy(cfg->client_sign_key_path, val);
        else if (strcmp(key, "ClientSignKeyPassword") == 0) strcpy(cfg->client_sign_key_password, val);
        else if (strcmp(key, "ClientEncCertPath") == 0) strcpy(cfg->client_enc_cert_path, val);
        else if (strcmp(key, "ClientEncKeyPath") == 0) strcpy(cfg->client_enc_key_path, val);

        else if (strcmp(key, "EnableDataValidation") == 0) cfg->enable_data_validation = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "EnableDetailLog") == 0) cfg->enable_detail_log = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcmp(key, "ResumableTaskNum") == 0) cfg->resumable_task_num = atoi(val);
    }
    
    if (cfg->part_size <= 0) cfg->part_size = 5 * 1024 * 1024; 
    if (cfg->target_user_count <= 0) {
        printf("[Config Error] 'Users' must be greater than 0.\n");
        fclose(fp); return -1;
    }

    if (cfg->test_case == TEST_CASE_MIX) {
        if (cfg->mix_op_count > 0) cfg->use_mix_mode = 1;
    } else {
        cfg->use_mix_mode = 0;
    }

    // -----------------------------------------------------------
    // 国密及双向认证严格校验
    // -----------------------------------------------------------
    if (strlen(cfg->gm_auth_mode) > 0) {
        if (strcasecmp(cfg->protocol, "https") != 0) {
            printf("[Config Error] Protocol MUST be 'https' when GmAuthMode is configured.\n");
            fclose(fp); return -1;
        }

        if (strcmp(cfg->gm_auth_mode, "GM_Mutual") == 0) {
            if (strlen(cfg->server_cert_path) == 0 || strlen(cfg->client_sign_cert_path) == 0 || 
                strlen(cfg->client_sign_key_path) == 0 || strlen(cfg->client_enc_cert_path) == 0 || 
                strlen(cfg->client_enc_key_path) == 0) {
                printf("[Config Error] Required certificates/keys missing for GM_Mutual.\n");
                fclose(fp); return -1;
            }
        } else if (strcmp(cfg->gm_auth_mode, "GM_Oneway") == 0) {
            if (strlen(cfg->server_cert_path) == 0) {
                printf("[Config Error] ServerCertPath missing for GM_Oneway.\n");
                fclose(fp); return -1;
            }
        } else if (strcmp(cfg->gm_auth_mode, "Inter_Mutual") == 0) {
            if (strlen(cfg->server_cert_path) == 0 || strlen(cfg->client_sign_cert_path) == 0 || 
                strlen(cfg->client_sign_key_path) == 0) {
                printf("[Config Error] Required certificates/keys missing for Inter_Mutual.\n");
                fclose(fp); return -1;
            }
        } else if (strcmp(cfg->gm_auth_mode, "Inter_Oneway") == 0) {
            if (strlen(cfg->server_cert_path) == 0) {
                printf("[Config Error] ServerCertPath missing for Inter_Oneway.\n");
                fclose(fp); return -1;
            }
        }
    }

    fclose(fp);
    snprintf(cfg->config_file_path, sizeof(cfg->config_file_path), "%s", filename);
    return 0;
}
