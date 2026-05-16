#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <mbedtls/version.h>
#include <sqlite3.h>
#include <stumpless/version.h>
#include <uv.h>
#ifdef TP_CMOCKA_LINK
#include <cmocka_version.h>
#endif
#include <libconfig.h>

#include "config.h"
#include "log.h"
#include "server.h"

#define VERSION "ppmxmpp dev build"

static void print_version(void) {
  printf("%s\n\n", VERSION);
  printf("Third-party library versions:\n");
  printf("  mbedtls:    %s  (%s)\n", mbedtls_version_get_string(), TP_MBEDTLS_LINK);
  printf("  libuv:      %s  (%s)\n", uv_version_string(), TP_LIBUV_LINK);
  printf("  libstrophe: 0.14.0  (%s)\n", TP_LIBSTROPHE_LINK);
  printf("  sqlite:     %s  (%s)\n", sqlite3_libversion(), TP_SQLITE_LINK);

  struct stumpless_version* sv = stumpless_get_version();
  printf("  stumpless:  %d.%d.%d  (%s)\n", sv->major, sv->minor, sv->patch, TP_STUMPLESS_LINK);
  free(sv);

#ifdef TP_CMOCKA_LINK
  printf("  cmocka:     %d.%d.%d  (%s)\n", CMOCKA_VERSION_MAJOR, CMOCKA_VERSION_MINOR,
         CMOCKA_VERSION_MICRO, TP_CMOCKA_LINK);
#else
  printf("  cmocka:     not built  (%s)\n", TP_CMOCKA_LINK);
#endif
  printf("  libconfig:  %d.%d.%d  (%s)\n", LIBCONFIG_VER_MAJOR, LIBCONFIG_VER_MINOR,
         LIBCONFIG_VER_REVISION, TP_LIBCONFIG_LINK);
}

static void print_help(const char* progname) {
  printf("Usage: %s [OPTIONS]\n\n", progname);
  printf("Options:\n");
  printf("  --config <file>       Configuration file path (default: %s)\n", DEFAULT_CONFIG);
  printf("  --db-path <path>      Database file path (default: %s)\n", server_config.db_path);
  printf("  --log-level <level>   Log level: TRACE, DEBUG, INFO, WARN, ERROR, "
         "FATAL\n");
  printf("  --version             Show version and third-party library "
         "versions\n");
  printf("  --help                Show this help message\n");
}

int main(int argc, char* argv[]) {
  server_config = config_parse_default_config();

  const char* config_path = DEFAULT_CONFIG;
  int cli_config = 0;
  const char* cli_db_path = NULL;
  const char* cli_log_level = NULL;

  static const struct option long_options[] = {{"config", required_argument, NULL, 'c'},
                                               {"db-path", required_argument, NULL, 'd'},
                                               {"log-level", required_argument, NULL, 'l'},
                                               {"version", no_argument, NULL, 'v'},
                                               {"help", no_argument, NULL, 'h'},
                                               {NULL, 0, NULL, 0}};

  log_init();

  int opt;
  while ((opt = getopt_long(argc, argv, "c:d:lv", long_options, NULL)) != -1) {
    switch (opt) {
    case 'c':
      config_path = optarg;
      cli_config = 1;
      break;
    case 'd':
      cli_db_path = optarg;
      break;
    case 'l':
      cli_log_level = optarg;
      break;
    case 'v':
      print_version();
      log_free();
      return 0;
    case 'h':
      print_help(argv[0]);
      log_free();
      return 0;
    default:
      stump_er("unknown option");
      print_help(argv[0]);
      log_free();
      return 1;
    }
  }

  if (access(config_path, F_OK) != 0) {
    if (cli_config) {
      stump_er("config file not found: %s", config_path);
      log_free();
      return 1;
    }
    if (config_create_default(config_path) != 0) {
      stump_er("failed to create default config: %s", config_path);
      log_free();
      return 1;
    }
  }

  if (config_load(config_path) != 0) {
    stump_er("failed to load config: %s", config_path);
    log_free();
    return 1;
  }

  if (cli_log_level && config_set_log_level(cli_log_level, &server_config) != 0) {
    stump_er("invalid --log-level: %s", cli_log_level);
    log_free();
    return 1;
  }

  if (cli_db_path && config_set_db_path(cli_db_path, &server_config) != 0) {
    stump_er("invalid --db-path: %s", cli_db_path);
    log_free();
    return 1;
  }

  config_print(config_path, &server_config);

  if (server_init() != 0) {
    stump_er("server_init failed");
    return 1;
  }

  uv_loop_t loop;
  uv_loop_init(&loop);

  int srv = server_start(&loop);
  if (srv != 0) {
    stump_er("server initialization failed");
    uv_loop_close(&loop);
    server_shutdown();
    return 1;
  }

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);

  server_shutdown();
  return 0;
}