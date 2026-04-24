#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include <mbedtls/version.h>
#include <uv.h>
#include <sqlite3.h>
#include <stumpless/version.h>
#include <cmocka_version.h>
#include <libconfig.h>

#include "config.h"
#include "log.h"

#define VERSION "xmpp-server dev build"

static void print_version(void) {
    printf("%s\n\n", VERSION);
    printf("Third-party library versions:\n");
    printf("  mbedtls:    %s  (%s)\n", mbedtls_version_get_string(), TP_MBEDTLS_LINK);
    printf("  libuv:      %s  (%s)\n", uv_version_string(),           TP_LIBUV_LINK);
    printf("  libstrophe: 0.14.0  (%s)\n",                            TP_LIBSTROPHE_LINK);
    printf("  sqlite:     %s  (%s)\n", sqlite3_libversion(),          TP_SQLITE_LINK);

    struct stumpless_version *sv = stumpless_get_version();
    printf("  stumpless:  %d.%d.%d  (%s)\n",
           sv->major, sv->minor, sv->patch, TP_STUMPLESS_LINK);
    free(sv);

    printf("  cmocka:     %d.%d.%d  (%s)\n",
           CMOCKA_VERSION_MAJOR, CMOCKA_VERSION_MINOR, CMOCKA_VERSION_MICRO,
           TP_CMOCKA_LINK);
    printf("  libconfig:  %d.%d.%d  (%s)\n",
           LIBCONFIG_VER_MAJOR, LIBCONFIG_VER_MINOR, LIBCONFIG_VER_REVISION,
           TP_LIBCONFIG_LINK);
}

static void print_help(const char *progname) {
    printf("Usage: %s [OPTIONS]\n\n", progname);
    printf("Options:\n");
    printf("  --config <file>       Configuration file path (default: %s)\n", DEFAULT_CONFIG);
    printf("  --log-level <level>   Log level: TRACE, DEBUG, INFO, WARN, ERROR, FATAL\n");
    printf("  --version             Show version and third-party library versions\n");
    printf("  --help                Show this help message\n");
}

int main(int argc, char *argv[]) {
    const char *config_path = DEFAULT_CONFIG;
    const char *cli_log_level = NULL;

    static const struct option long_options[] = {
        {"config",    required_argument, NULL, 'c'},
        {"log-level", required_argument, NULL, 'l'},
        {"version",   no_argument,       NULL, 'v'},
        {"help",      no_argument,       NULL, 'h'},
        {NULL,        0,                 NULL,  0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'l':
            if (config_parse_log_level(optarg) < 0) {
                stump_er("invalid log level '%s' (valid: TRACE DEBUG INFO WARN ERROR FATAL)",
                         optarg);
                return 1;
            }
            cli_log_level = optarg;
            break;
        case 'v': print_version(); return 0;
        case 'h': print_help(argv[0]); return 0;
        default:  print_help(argv[0]); return 1;
        }
    }

    log_init();

    server_config_t cfg;
    if (config_load(config_path, cli_log_level, &cfg) != 0) {
        log_free();
        return 1;
    }

    config_print(config_path, &cfg);

    log_free();
    return 0;
}
