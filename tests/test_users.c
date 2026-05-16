#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "storage/db.h"
#include "storage/db_users.h"

/* ---------------------------------------------------------------------------
 * Helper: generate a unique temp path for the database.
 * ------------------------------------------------------------------------ */
static void gen_db_path(char* buf, size_t len) {
  snprintf(buf, len, "/tmp/test_xmpp_users_%d_%d_%d.db", getpid(), (int)time(NULL), rand());
}

/* ---------------------------------------------------------------------------
 * Setup / Teardown
 * ------------------------------------------------------------------------ */
static int users_test_setup(void** state) {
  char* path = malloc(512);
  assert_non_null(path);
  gen_db_path(path, 512);
  *state = path;
  return 0;
}

static int users_test_teardown(void** state) {
  if (*state) {
    unlink(*state);
    free(*state);
    *state = NULL;
  }
  storage_db_close();
  return 0;
}

/* ---------------------------------------------------------------------------
 * Helper: load config and open DB. Returns sqlite3* or NULL on failure.
 * ------------------------------------------------------------------------ */
static sqlite3* open_test_db(const char* db_path) {
  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "/tmp/test_xmpp_conf_%d_%d.conf", getpid(),
           (int)time(NULL));

  FILE* f = fopen(conf_path, "w");
  if (!f) {
    return NULL;
  }
  fprintf(f, "db_path = \"%s\";\n", db_path);
  fprintf(f, "log_level = \"ERROR\";\n");
  fprintf(f, "bind_host = \"127.0.0.1\";\n");
  fprintf(f, "bind_port = 5222;\n");
  fclose(f);

  server_config = config_parse_default_config();
  int rc = config_load(conf_path);
  unlink(conf_path);
  if (rc != 0) {
    return NULL;
  }

  sqlite3* db = NULL;
  if (storage_db_open(&db) != 0) {
    return NULL;
  }
  return db;
}

/* ---------------------------------------------------------------------------
 * storage_users_create tests
 * ------------------------------------------------------------------------ */

/* Creating a new user should succeed and return 0. */
static void test_users_create_success(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_create("alice@localhost", "alicepass");
  assert_int_equal(rc, 0);

  storage_db_close();
}

/* Creating a user with empty password should succeed (no validation in create).
 */
static void test_users_create_empty_password(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_create("bob@localhost", "");
  assert_int_equal(rc, 0);

  storage_db_close();
}

/* Creating a user with very long JID should succeed. */
static void test_users_create_long_jid(void** state) {
  char* path = *state;
  open_test_db(path);

  char long_jid[512];
  memset(long_jid, 'a', sizeof(long_jid) - 64);
  strcpy(long_jid + sizeof(long_jid) - 64, "@example.com");
  long_jid[sizeof(long_jid) - 1] = '\0';

  int rc = storage_users_create(long_jid, "pass");
  assert_int_equal(rc, 0);

  storage_db_close();
}

/* Creating a user with NULL jid should fail. */
static void test_users_create_null_jid(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_create(NULL, "pass");
  assert_int_equal(rc, -1);

  storage_db_close();
}

/* Creating a user with NULL password should fail. */
static void test_users_create_null_password(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_create("user@localhost", NULL);
  assert_int_equal(rc, -1);

  storage_db_close();
}

/* Creating a duplicate user (same JID) should fail. */
static void test_users_create_duplicate(void** state) {
  char* path = *state;
  open_test_db(path);

  assert_int_equal(storage_users_create("dup@localhost", "pass1"), 0);
  assert_int_equal(storage_users_create("dup@localhost", "pass2"), -1);

  storage_db_close();
}

/* ---------------------------------------------------------------------------
 * storage_users_get_by_jid tests
 * ------------------------------------------------------------------------ */

/* Getting an existing user should return 0 and fill buffers. */
static void test_users_get_existing(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_users_create("getuser@localhost", "getpass");

  storage_user_t user;
  int rc = storage_users_get_by_jid("getuser@localhost", &user);
  assert_int_equal(rc, 0);
  assert_string_equal(user.jid, "getuser@localhost");
  assert_string_equal(user.password_plain, "getpass");

  storage_db_close();
}

/* Getting a non-existent user should return 1. */
static void test_users_get_nonexistent(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_user_t user;
  int rc = storage_users_get_by_jid("noone@localhost", &user);
  assert_int_equal(rc, 1);

  storage_db_close();
}

/* Getting a user with NULL output should fail. */
static void test_users_get_null_output(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_users_create("nullout@localhost", "pass");

  int rc = storage_users_get_by_jid("nullout@localhost", NULL);
  assert_int_equal(rc, -1);

  storage_db_close();
}

/* Getting a user with NULL jid should fail. */
static void test_users_get_null_jid(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_user_t user;
  int rc = storage_users_get_by_jid(NULL, &user);
  assert_int_equal(rc, -1);

  storage_db_close();
}

/* ---------------------------------------------------------------------------
 * storage_users_check_password tests
 * ------------------------------------------------------------------------ */

/* Checking correct password should return 1. */
static void test_users_check_password_correct(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_users_create("passuser@localhost", "correctpass");

  int rc = storage_users_check_password("passuser@localhost", "correctpass");
  assert_int_equal(rc, 1);

  storage_db_close();
}

/* Checking wrong password should return 0. */
static void test_users_check_password_wrong(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_users_create("passuser2@localhost", "correctpass");

  int rc = storage_users_check_password("passuser2@localhost", "wrongpass");
  assert_int_equal(rc, 0);

  storage_db_close();
}

/* Checking password for non-existent user should return 0. */
static void test_users_check_password_nonexistent(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_check_password("noone@localhost", "pass");
  assert_int_equal(rc, 0);

  storage_db_close();
}

/* Checking with NULL jid should return 0. */
static void test_users_check_password_null_jid(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_check_password(NULL, "pass");
  assert_int_equal(rc, 0);

  storage_db_close();
}

/* Checking with NULL password should return 0. */
static void test_users_check_password_null_password(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_users_create("nullpass@localhost", "pass");

  int rc = storage_users_check_password("nullpass@localhost", NULL);
  assert_int_equal(rc, 0);

  storage_db_close();
}

/* ---------------------------------------------------------------------------
 * storage_users_disable tests
 * ------------------------------------------------------------------------ */

/* Disabling an existing user should succeed. */
static void test_users_disable_success(void** state) {
  char* path = *state;
  open_test_db(path);

  storage_users_create("disableuser@localhost", "pass");

  int rc = storage_users_disable("disableuser@localhost");
  assert_int_equal(rc, 0);

  /* Verify the user is now disabled (get_by_jid returns 1 for disabled). */
  storage_user_t user;
  int rc2 = storage_users_get_by_jid("disableuser@localhost", &user);
  assert_int_equal(rc2, 1);

  storage_db_close();
}

/* Disabling a non-existent user should fail. */
static void test_users_disable_nonexistent(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_disable("noone@localhost");
  assert_int_equal(rc, -1);

  storage_db_close();
}

/* Disabling with NULL jid should fail. */
static void test_users_disable_null_jid(void** state) {
  char* path = *state;
  open_test_db(path);

  int rc = storage_users_disable(NULL);
  assert_int_equal(rc, -1);

  storage_db_close();
}

/* ---------------------------------------------------------------------------
 * Integration: create -> get -> disable -> verify
 * ------------------------------------------------------------------------ */

/* Full lifecycle test: create, retrieve, disable, verify. */
static void test_users_full_lifecycle(void** state) {
  char* path = *state;
  open_test_db(path);

  /* 1. Create user. */
  assert_int_equal(storage_users_create("lifecycle@localhost", "pass1"), 0);

  /* 2. Retrieve user. */
  storage_user_t user;
  assert_int_equal(storage_users_get_by_jid("lifecycle@localhost", &user), 0);
  assert_string_equal(user.jid, "lifecycle@localhost");
  assert_string_equal(user.password_plain, "pass1");

  /* 3. Verify password. */
  assert_int_equal(storage_users_check_password("lifecycle@localhost", "pass1"), 1);

  /* 4. Disable user. */
  assert_int_equal(storage_users_disable("lifecycle@localhost"), 0);

  /* 5. Verify disabled (get returns 1 = not found). */
  assert_int_equal(storage_users_get_by_jid("lifecycle@localhost", &user), 1);

  storage_db_close();
}

/* ---------------------------------------------------------------------------
 * Null-safety tests
 * ------------------------------------------------------------------------ */

/* All user functions should handle NULL gracefully (no crash). */
static void test_users_null_safety(void** state) {
  (void)state;
  /* These should not crash; behavior may vary but shouldn't segfault. */
  storage_users_create(NULL, "pass");
  storage_users_create("user@localhost", NULL);
  storage_users_check_password(NULL, "pass");
  storage_users_check_password("user@localhost", NULL);
  storage_users_disable(NULL);
}

/* ---------------------------------------------------------------------------
 * Test group with setup / teardown
 * ------------------------------------------------------------------------ */

static void test_users_db_path_configured(void** state) {
  char* path = *state;
  open_test_db(path);
  storage_db_close();
}

/* ---------------------------------------------------------------------------
 * Main — register all tests
 * ------------------------------------------------------------------------ */
int main(void) {
  const struct CMUnitTest tests[] = {
      /* Create tests. */
      cmocka_unit_test_setup_teardown(test_users_create_success, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_create_empty_password, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_create_long_jid, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_create_null_jid, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_create_null_password, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_create_duplicate, users_test_setup,
                                      users_test_teardown),

      /* Get tests. */
      cmocka_unit_test_setup_teardown(test_users_get_existing, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_get_nonexistent, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_get_null_output, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_get_null_jid, users_test_setup,
                                      users_test_teardown),

      /* Check password tests. */
      cmocka_unit_test_setup_teardown(test_users_check_password_correct, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_check_password_wrong, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_check_password_nonexistent, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_check_password_null_jid, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_check_password_null_password, users_test_setup,
                                      users_test_teardown),

      /* Disable tests. */
      cmocka_unit_test_setup_teardown(test_users_disable_success, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_disable_nonexistent, users_test_setup,
                                      users_test_teardown),
      cmocka_unit_test_setup_teardown(test_users_disable_null_jid, users_test_setup,
                                      users_test_teardown),

      /* Integration test. */
      cmocka_unit_test_setup_teardown(test_users_full_lifecycle, users_test_setup,
                                      users_test_teardown),

      /* Null-safety tests. */
      cmocka_unit_test(test_users_null_safety),

      /* Setup/teardown group. */
      cmocka_unit_test_setup_teardown(test_users_db_path_configured, users_test_setup,
                                      users_test_teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
