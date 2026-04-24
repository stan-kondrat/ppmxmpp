#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

#define TMPCONF "/tmp/test_xmpp.conf"

static void write_conf(const char *content) {
    FILE *f = fopen(TMPCONF, "w");
    fputs(content, f);
    fclose(f);
}

/* --- config_parse_log_level --- */

static void test_parse_all_valid_levels(void **state) {
    (void)state;
    assert_int_not_equal(config_parse_log_level("TRACE"), -1);
    assert_int_not_equal(config_parse_log_level("DEBUG"), -1);
    assert_int_not_equal(config_parse_log_level("INFO"),  -1);
    assert_int_not_equal(config_parse_log_level("WARN"),  -1);
    assert_int_not_equal(config_parse_log_level("ERROR"), -1);
    assert_int_not_equal(config_parse_log_level("FATAL"), -1);
}

static void test_parse_case_insensitive(void **state) {
    (void)state;
    assert_int_not_equal(config_parse_log_level("info"),  -1);
    assert_int_not_equal(config_parse_log_level("Debug"), -1);
    assert_int_not_equal(config_parse_log_level("wArN"),  -1);
}

static void test_parse_invalid_level(void **state) {
    (void)state;
    assert_int_equal(config_parse_log_level("VERBOSE"), -1);
    assert_int_equal(config_parse_log_level(""),        -1);
    assert_int_equal(config_parse_log_level("none"),    -1);
}

/* --- config_load: no config file, no arg --- */

static void test_no_file_no_arg_uses_default(void **state) {
    (void)state;
    unlink(TMPCONF);

    server_config_t cfg;
    assert_int_equal(config_load(TMPCONF, NULL, &cfg), 0);
    assert_string_equal(cfg.log_level, DEFAULT_LOG_LEVEL);

    unlink(TMPCONF);
}

static void test_no_file_no_arg_creates_file(void **state) {
    (void)state;
    unlink(TMPCONF);

    server_config_t cfg;
    config_load(TMPCONF, NULL, &cfg);
    assert_int_equal(access(TMPCONF, F_OK), 0);

    unlink(TMPCONF);
}

/* --- config_load: config file present, no arg --- */

static void test_file_log_level_read(void **state) {
    (void)state;
    write_conf("log_level = \"DEBUG\";\n");

    server_config_t cfg;
    assert_int_equal(config_load(TMPCONF, NULL, &cfg), 0);
    assert_string_equal(cfg.log_level, "DEBUG");

    unlink(TMPCONF);
}

static void test_file_missing_key_uses_default(void **state) {
    (void)state;
    write_conf("# no log_level here\n");

    server_config_t cfg;
    assert_int_equal(config_load(TMPCONF, NULL, &cfg), 0);
    assert_string_equal(cfg.log_level, DEFAULT_LOG_LEVEL);

    unlink(TMPCONF);
}

/* --- config_load: no config file, arg provided --- */

static void test_no_file_arg_wins(void **state) {
    (void)state;
    unlink(TMPCONF);

    server_config_t cfg;
    assert_int_equal(config_load(TMPCONF, "WARN", &cfg), 0);
    assert_string_equal(cfg.log_level, "WARN");

    unlink(TMPCONF);
}

/* --- config_load: config file present, arg provided --- */

static void test_file_and_arg_arg_wins(void **state) {
    (void)state;
    write_conf("log_level = \"DEBUG\";\n");

    server_config_t cfg;
    assert_int_equal(config_load(TMPCONF, "ERROR", &cfg), 0);
    assert_string_equal(cfg.log_level, "ERROR");

    unlink(TMPCONF);
}

/* --- error cases --- */

static void test_invalid_level_in_file_fails(void **state) {
    (void)state;
    write_conf("log_level = \"BOGUS\";\n");

    server_config_t cfg;
    assert_int_equal(config_load(TMPCONF, NULL, &cfg), -1);

    unlink(TMPCONF);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_all_valid_levels),
        cmocka_unit_test(test_parse_case_insensitive),
        cmocka_unit_test(test_parse_invalid_level),
        cmocka_unit_test(test_no_file_no_arg_uses_default),
        cmocka_unit_test(test_no_file_no_arg_creates_file),
        cmocka_unit_test(test_file_log_level_read),
        cmocka_unit_test(test_file_missing_key_uses_default),
        cmocka_unit_test(test_no_file_arg_wins),
        cmocka_unit_test(test_file_and_arg_arg_wins),
        cmocka_unit_test(test_invalid_level_in_file_fails),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
