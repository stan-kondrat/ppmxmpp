#ifndef CONFIG_H
#define CONFIG_H

#define DEFAULT_CONFIG "config/ppmxmpp.conf"

/* Embedded content of the default config file (generated at build time). */
extern const char DEFAULT_CONFIG_CONTENT[];

typedef struct {
    char db_path[512];
    char log_level[16];
} server_config_t;

/* The global server config, filled by config_load(). */
extern server_config_t server_config;

/* Parse DEFAULT_CONFIG_CONTENT and return the resulting defaults.
 * Must be called before config_load().
 * Exits on failure (invalid embedded content is a build error). */
server_config_t config_parse_default_config(void);

/* Create parent directories and write DEFAULT_CONFIG_CONTENT to path.
 * Returns 0 on success, -1 on error. */
int config_create_default(const char *path);

/* Load and parse the config file at path into server_config.
 * Returns 0 on success, -1 on error. */
int config_load(const char *path);

/* Print the resolved config file path and all settings. */
void config_print(const char *path, const server_config_t *cfg);

/* Validate and set db_path in out. Returns 0 on success, -1 on error. */
int config_set_db_path(const char *path, server_config_t *out);

/* Validate and set log_level in out. Returns 0 on success, -1 on error. */
int config_set_log_level(const char *level, server_config_t *out);

#endif /* CONFIG_H */
