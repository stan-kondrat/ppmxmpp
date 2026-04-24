#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#include <libconfig.h>

#include "config.h"
#include "log.h"

static const char *LOG_LEVELS[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", NULL
};

int config_parse_log_level(const char *level) {
    for (int i = 0; LOG_LEVELS[i] != NULL; i++) {
        if (strcasecmp(level, LOG_LEVELS[i]) == 0)
            return i;
    }
    return -1;
}

static int create_default_file(const char *path) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            stump_er("cannot create directory '%s': %s", dir, strerror(errno));
            return -1;
        }
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        stump_er("cannot create '%s': %s", path, strerror(errno));
        return -1;
    }
    fputs(DEFAULT_CONFIG_CONTENT, f);
    fclose(f);
    stump_i("created default config: %s", path);
    return 0;
}

int config_load(const char *path, const char *cli_log_level, server_config_t *out) {
    config_t cfg;
    config_init(&cfg);

    if (!config_read_file(&cfg, path)) {
        if (config_error_line(&cfg) == 0) {
            if (create_default_file(path) != 0) {
                config_destroy(&cfg);
                return -1;
            }
            if (!config_read_file(&cfg, path)) {
                stump_er("%s:%d - %s",
                         config_error_file(&cfg),
                         config_error_line(&cfg),
                         config_error_text(&cfg));
                config_destroy(&cfg);
                return -1;
            }
        } else {
            stump_er("%s:%d - %s",
                     config_error_file(&cfg),
                     config_error_line(&cfg),
                     config_error_text(&cfg));
            config_destroy(&cfg);
            return -1;
        }
    }

    const char *log_level = DEFAULT_LOG_LEVEL;
    if (cli_log_level) {
        log_level = cli_log_level;
    } else {
        const char *val;
        if (config_lookup_string(&cfg, "log_level", &val)) {
            if (config_parse_log_level(val) < 0) {
                stump_er("invalid log_level '%s' in %s", val, path);
                config_destroy(&cfg);
                return -1;
            }
            log_level = val;
        }
    }

    snprintf(out->log_level, sizeof(out->log_level), "%s", log_level);
    config_destroy(&cfg);
    return 0;
}

void config_print(const char *path, const server_config_t *cfg) {
    char full_path[PATH_MAX];
    if (realpath(path, full_path) == NULL)
        snprintf(full_path, sizeof(full_path), "%s", path);

    printf("Config file: %s\n", full_path);
    printf("  log_level = %s\n", cfg->log_level);
}
