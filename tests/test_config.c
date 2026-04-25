#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include "config.h"

/* ---------------------------------------------------------------------------
 * Helper: write a config file at the given path with the supplied content.
 * ------------------------------------------------------------------------ */
static void write_conf_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(content, f);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Helper: remove a config file if it exists (silent on error).
 * ------------------------------------------------------------------------ */
static void remove_conf_file(const char *path) {
    unlink(path);
}

/* ---------------------------------------------------------------------------
 * Helper: remove a directory if it exists (silent on error).
 * ------------------------------------------------------------------------ */
static void remove_dir(const char *dir) {
    rmdir(dir);
}

/* ---------------------------------------------------------------------------
 * Setup / Teardown
 *
 * Each test receives a `char *` in state that points to the temp path it
 * should use.  Setup allocates a unique path in /tmp; teardown removes the
 * file and frees the string.
 * ------------------------------------------------------------------------ */
static int config_test_setup(void **state) {
    char *path = malloc(512);
    assert_non_null(path);
    snprintf(path, 512, "/tmp/test_xmpp_%d_%d.conf", getpid(), (int)time(NULL));
    *state = path;
    return 0;
}

static int config_test_teardown(void **state) {
    if (*state) {
        remove_conf_file(*state);
        free(*state);
        *state = NULL;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * config_set_log_level tests
 * ------------------------------------------------------------------------ */
static void test_parse_all_valid_levels(void **state) {
    (void)state;
    server_config_t out;
    assert_int_equal(config_set_log_level("TRACE", &out), 0);
    assert_int_equal(config_set_log_level("DEBUG", &out), 0);
    assert_int_equal(config_set_log_level("INFO",  &out), 0);
    assert_int_equal(config_set_log_level("WARN",  &out), 0);
    assert_int_equal(config_set_log_level("ERROR", &out), 0);
    assert_int_equal(config_set_log_level("FATAL", &out), 0);
}

static void test_parse_case_insensitive(void **state) {
    (void)state;
    server_config_t out;
    assert_int_equal(config_set_log_level("info",  &out), 0);
    assert_int_equal(config_set_log_level("Debug", &out), 0);
    assert_int_equal(config_set_log_level("wArN",  &out), 0);
}

static void test_parse_invalid_level(void **state) {
    (void)state;
    server_config_t out;
    assert_int_equal(config_set_log_level("VERBOSE", &out), -1);
    assert_int_equal(config_set_log_level("",        &out), -1);
    assert_int_equal(config_set_log_level("none",    &out), -1);
}

/* ---------------------------------------------------------------------------
 * config_load: config creation — no file exists, no CLI arg
 *
 * These tests verify that config_load creates a default config file when
 * the path does not exist, and that the created file contains valid values.
 * ------------------------------------------------------------------------ */

/* The config file is created in /tmp and contains the default log level. */
static void test_config_created_in_tmp_with_defaults(void **state) {
    (void)state;
    char path[512];
    snprintf(path, sizeof(path), "/tmp/test_xmpp_%d_%d.conf", getpid(), (int)time(NULL));
    remove_conf_file(path);

    server_config = config_parse_default_config();
    assert_int_equal(config_create_default(path), 0);
    assert_int_equal(access(path, F_OK), 0);
    assert_int_equal(config_load(path), 0);
    assert_string_equal(server_config.log_level, "INFO");
    remove_conf_file(path);
}

/* Config creation succeeds even when the parent directory does not exist
 * — config_create_default should create the directory hierarchy. */
static void test_config_created_with_nested_dir(void **state) {
    (void)state;
    char path[512];
    char dir[512];
    snprintf(path, sizeof(path), "/tmp/test_xmpp_%d_%d/subdir/config.conf",
             getpid(), (int)time(NULL));

    /* Extract directory for cleanup */
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    server_config = config_parse_default_config();
    assert_int_equal(config_create_default(path), 0);
    assert_int_equal(access(path, F_OK), 0);
    assert_int_equal(config_load(path), 0);
    assert_string_equal(server_config.log_level, "INFO");

    /* Clean up nested dir */
    unlink(path);
    remove_dir(dir);
}

/* ---------------------------------------------------------------------------
 * config_load: config file already exists, no CLI arg
 *
 * Verifies that an existing config file is parsed correctly.
 * ------------------------------------------------------------------------ */

static void test_existing_config_log_level_read(void **state) {
    char *path = *state;
    write_conf_file(path, "log_level = \"DEBUG\";\n");

    server_config = config_parse_default_config();
    assert_int_equal(config_load(path), 0);
    assert_string_equal(server_config.log_level, "DEBUG");
}

static void test_existing_config_missing_key_uses_default(void **state) {
    char *path = *state;
    write_conf_file(path, "# no log_level here\n");

    server_config = config_parse_default_config();
    assert_int_equal(config_load(path), 0);
    assert_string_equal(server_config.log_level, "INFO");
}

/* ---------------------------------------------------------------------------
 * config_load: CLI log level overrides config file
 * ------------------------------------------------------------------------ */

static void test_cli_arg_overrides_file(void **state) {
    char *path = *state;
    write_conf_file(path, "log_level = \"DEBUG\";\n");

    server_config = config_parse_default_config();
    assert_int_equal(config_load(path), 0);
    assert_int_equal(config_set_log_level("ERROR", &server_config), 0);
    assert_string_equal(server_config.log_level, "ERROR");
}

/* When no file exists, CLI arg still wins and a default file is created. */
static void test_cli_arg_wins_no_file(void **state) {
    char *path = *state;
    remove_conf_file(path);

    server_config = config_parse_default_config();
    assert_int_equal(config_create_default(path), 0);
    assert_int_equal(access(path, F_OK), 0);
    assert_int_equal(config_load(path), 0);
    assert_int_equal(config_set_log_level("WARN", &server_config), 0);
    assert_string_equal(server_config.log_level, "WARN");
}

/* ---------------------------------------------------------------------------
 * config_load: error cases
 * ------------------------------------------------------------------------ */

static void test_invalid_log_level_in_file_fails(void **state) {
    char *path = *state;
    write_conf_file(path, "log_level = \"BOGUS\";\n");

    server_config = config_parse_default_config();
    assert_int_equal(config_load(path), -1);
}

/* ---------------------------------------------------------------------------
 * Test group with setup / teardown
 * ------------------------------------------------------------------------ */

/* Each test in this group receives the temp path via state. */
static void test_config_in_tmp_created(void **state) {
    char *path = *state;
    remove_conf_file(path);

    server_config = config_parse_default_config();
    assert_int_equal(config_create_default(path), 0);
    assert_int_equal(access(path, F_OK), 0);
    assert_int_equal(config_load(path), 0);
    assert_string_equal(server_config.log_level, "INFO");
}

static void test_config_in_tmp_overwritten(void **state) {
    char *path = *state;
    write_conf_file(path, "log_level = \"TRACE\";\n");

    server_config = config_parse_default_config();
    assert_int_equal(config_load(path), 0);
    assert_string_equal(server_config.log_level, "TRACE");
}

/* ---------------------------------------------------------------------------
 * Main — register all tests
 * ------------------------------------------------------------------------ */
int main(void) {
    const struct CMUnitTest tests[] = {
        /* Parse log level (no setup/teardown needed). */
        cmocka_unit_test(test_parse_all_valid_levels),
        cmocka_unit_test(test_parse_case_insensitive),
        cmocka_unit_test(test_parse_invalid_level),

        /* E2E config creation — no setup/teardown (uses global /tmp). */
        cmocka_unit_test(test_config_created_with_nested_dir),

        /* Tests with setup/teardown. */
        cmocka_unit_test_setup_teardown(test_config_created_in_tmp_with_defaults,
                                        config_test_setup, config_test_teardown),
        cmocka_unit_test_setup_teardown(test_existing_config_log_level_read,
                                        config_test_setup, config_test_teardown),
        cmocka_unit_test_setup_teardown(test_existing_config_missing_key_uses_default,
                                        config_test_setup, config_test_teardown),
        cmocka_unit_test_setup_teardown(test_cli_arg_overrides_file,
                                        config_test_setup, config_test_teardown),
        cmocka_unit_test_setup_teardown(test_cli_arg_wins_no_file,
                                        config_test_setup, config_test_teardown),
        cmocka_unit_test_setup_teardown(test_invalid_log_level_in_file_fails,
                                        config_test_setup, config_test_teardown),
        cmocka_unit_test_setup_teardown(test_config_in_tmp_created,
                                        config_test_setup, config_test_teardown),
        cmocka_unit_test_setup_teardown(test_config_in_tmp_overwritten,
                                        config_test_setup, config_test_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
