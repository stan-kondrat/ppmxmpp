#ifndef CONFIG_H
#define CONFIG_H

#define DEFAULT_CONFIG    "config/xmpp.conf"
#define DEFAULT_LOG_LEVEL "INFO"

/* Embedded content of the default config file (generated at build time). */
extern const char DEFAULT_CONFIG_CONTENT[];

typedef struct {
    char log_level[16];
} server_config_t;

/* Load config from path, applying cli_log_level override if non-NULL.
 * Creates the file with defaults if it does not exist.
 * Returns 0 on success, -1 on error. */
int config_load(const char *path, const char *cli_log_level, server_config_t *out);

/* Print the resolved config file path and all settings. */
void config_print(const char *path, const server_config_t *cfg);

/* Returns index >= 0 if level is valid, -1 otherwise. */
int config_parse_log_level(const char *level);

#endif /* CONFIG_H */
