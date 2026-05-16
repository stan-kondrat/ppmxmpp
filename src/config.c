#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libconfig.h>

#include "config.h"
#include "log.h"

static const char* LOG_LEVELS[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", NULL};

static server_config_t default_server_config;
server_config_t server_config;

int config_set_db_path(const char* path, server_config_t* out) {
  if (!path || path[0] == '\0') {
    stump_er("db_path must not be empty");
    return -1;
  }
  (void)snprintf(out->db_path, sizeof(out->db_path), "%s", path);
  return 0;
}

int config_set_log_level(const char* level, server_config_t* out) {
  for (int i = 0; LOG_LEVELS[i] != NULL; i++) {
    if (strcasecmp(level, LOG_LEVELS[i]) == 0) {
      (void)snprintf(out->log_level, sizeof(out->log_level), "%s", LOG_LEVELS[i]);
      return 0;
    }
  }
  stump_er("invalid log_level '%s'", level);
  return -1;
}

int config_set_bind_host(const char* host, server_config_t* out) {
  if (!host || host[0] == '\0') {
    stump_er("bind_host must not be empty");
    return -1;
  }
  (void)snprintf(out->bind_host, sizeof(out->bind_host), "%s", host);
  return 0;
}

int config_set_bind_port(int port, server_config_t* out) {
  if (port < 1 || port > 65535) {
    stump_er("bind_port %d out of range [1, 65535]", port);
    return -1;
  }
  out->bind_port = port;
  return 0;
}

int config_set_tls_cert_file(const char* path, server_config_t* out) {
  if (!path || path[0] == '\0') {
    (void)snprintf(out->tls_cert_file, sizeof(out->tls_cert_file), "%s", "");
    return 0;
  }
  (void)snprintf(out->tls_cert_file, sizeof(out->tls_cert_file), "%s", path);
  return 0;
}

int config_set_tls_key_file(const char* path, server_config_t* out) {
  if (!path || path[0] == '\0') {
    (void)snprintf(out->tls_key_file, sizeof(out->tls_key_file), "%s", "");
    return 0;
  }
  (void)snprintf(out->tls_key_file, sizeof(out->tls_key_file), "%s", path);
  return 0;
}

static int config_parse_cfg(config_t* cfg, server_config_t* out) {
  const char* val;
  int ival;

  if (config_lookup_string(cfg, "db_path", &val) && config_set_db_path(val, out) != 0) {
    stump_er("config: invalid db_path");
    return -1;
  }

  if (config_lookup_string(cfg, "log_level", &val) && config_set_log_level(val, out) != 0) {
    stump_er("config: invalid log_level");
    return -1;
  }

  if (config_lookup_string(cfg, "bind_host", &val) && config_set_bind_host(val, out) != 0) {
    stump_er("config: invalid bind_host");
    return -1;
  }

  if (config_lookup_int(cfg, "bind_port", &ival) && config_set_bind_port(ival, out) != 0) {
    stump_er("config: invalid bind_port");
    return -1;
  }

  if (config_lookup_string(cfg, "tls_cert_file", &val)) {
    config_set_tls_cert_file(val, out);
  }

  if (config_lookup_string(cfg, "tls_key_file", &val)) {
    config_set_tls_key_file(val, out);
  }

  return 0;
}

server_config_t config_parse_default_config(void) {
  config_t cfg;
  config_init(&cfg);

  if (!config_read_string(&cfg, DEFAULT_CONFIG_CONTENT)) {
    (void)fprintf(stderr, "fatal: embedded default config is invalid: line %d - %s\n",
                  config_error_line(&cfg), config_error_text(&cfg));
    config_destroy(&cfg);
    exit(1);
  }

  default_server_config = (server_config_t){0};

  if (config_parse_cfg(&cfg, &default_server_config) != 0 ||
      default_server_config.db_path[0] == '\0' || default_server_config.log_level[0] == '\0' ||
      default_server_config.bind_host[0] == '\0' || default_server_config.bind_port == 0) {
    (void)fprintf(stderr, "fatal: embedded default config missing or invalid required fields\n");
    config_destroy(&cfg);
    exit(1);
  }

  config_destroy(&cfg);
  return default_server_config;
}

int config_create_default(const char* path) {
  char dir[256];
  (void)snprintf(dir, sizeof(dir), "%s", path);
  char* slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    for (char* p = dir + 1; *p; p++) {
      if (*p == '/') {
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
          stump_er("cannot create directory '%s': %s", dir, strerror(errno));
          return -1;
        }
        *p = '/';
      }
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
      stump_er("cannot create directory '%s': %s", dir, strerror(errno));
      return -1;
    }
  }

  FILE* f = fopen(path, "w");
  if (!f) {
    stump_er("cannot create '%s': %s", path, strerror(errno));
    return -1;
  }
  (void)fputs(DEFAULT_CONFIG_CONTENT, f);
  (void)fclose(f);
  stump_i("created default config: %s", path);
  return 0;
}

int config_load(const char* path) {
  config_t cfg;
  config_init(&cfg);

  if (!config_read_file(&cfg, path)) {
    stump_er("%s:%d - %s", config_error_file(&cfg), config_error_line(&cfg),
             config_error_text(&cfg));
    config_destroy(&cfg);
    return -1;
  }

  server_config = default_server_config;
  if (config_parse_cfg(&cfg, &server_config) != 0) {
    stump_er("config: parse failed");
    config_destroy(&cfg);
    return -1;
  }
  config_destroy(&cfg);
  return 0;
}

void config_print(const char* path, const server_config_t* cfg) {
  char full_path[PATH_MAX];
  if (realpath(path, full_path) == NULL) {
    (void)snprintf(full_path, sizeof(full_path), "%s", path);
  }

  stump_i("config: file=\"%s\" db_path=\"%s\" log_level=\"%s\" bind=\"%s:%d\" "
          "tls_cert=\"%s\" tls_key=\"%s\"",
          full_path, cfg->db_path, cfg->log_level, cfg->bind_host, cfg->bind_port,
          cfg->tls_cert_file, cfg->tls_key_file);
}
