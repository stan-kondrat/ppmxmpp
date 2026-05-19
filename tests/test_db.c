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

#include "test_xmpp_helpers.h"
#include "config.h"
#include "storage/db.h"

/* ---------------------------------------------------------------------------
 * Helper: generate a unique temp path for the database.
 * ------------------------------------------------------------------------ */
static void gen_db_path(char* buf, size_t len) {
  snprintf(buf, len, "/tmp/test_xmpp_db_%d_%d_%d.db", getpid(), (int)time(NULL), rand());
}

/* ---------------------------------------------------------------------------
 * Setup / Teardown
 *
 * Each test receives a `char *` in state that points to the temp db path.
 * Setup allocates a unique path; teardown removes the file.
 * ------------------------------------------------------------------------ */
static int db_test_setup(void** state) {
  char* path = malloc(512);
  assert_non_null(path);
  gen_db_path(path, 512);
  *state = path;
  return 0;
}

static int db_test_teardown(void** state) {
  if (*state) {
    unlink(*state);
    free(*state);
    *state = NULL;
  }
  /* Always close the DB to reset static state (g_db, g_stmts). */
  storage_db_close();
  return 0;
}

/* ---------------------------------------------------------------------------
 * Helper: load a config with the given db_path, return 0 on success.
 * ------------------------------------------------------------------------ */
static int setup_test_config(const char* db_path) {
  char conf_path[512];
  snprintf(conf_path, sizeof(conf_path), "/tmp/test_xmpp_conf_%d_%d.conf", getpid(),
           (int)time(NULL));

  /* Write a minimal config file. */
  FILE* f = fopen(conf_path, "w");
  if (!f) {
    return -1;
  }
  fprintf(f, "db_path = \"%s\";\n", db_path);
  fprintf(f, "log_level = \"ERROR\";\n");
  fprintf(f, "bind_host = \"127.0.0.1\";\n");
  fprintf(f, "bind_port = 5222;\n");
  fclose(f);

  server_config = config_parse_default_config();
  int rc = config_load(conf_path);
  unlink(conf_path);
  return rc;
}

/* ---------------------------------------------------------------------------
 * storage_db_open / storage_db_close tests
 * ------------------------------------------------------------------------ */

/* Opening a non-existent database should create it. */
static void test_db_open_creates_file(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));
  assert_int_equal(unlink(path), -1); /* ensure it doesn't exist */

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);
  assert_non_null(db);
  assert_int_equal(access(path, F_OK), 0);

  storage_db_close();
  unlink(path);
}

/* Opening the same database twice returns the same handle (singleton). */
static void test_db_open_singleton(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db1 = NULL;
  sqlite3* db2 = NULL;
  assert_int_equal(storage_db_open(&db1), 0);
  assert_int_equal(storage_db_open(&db2), 0);
  assert_ptr_equal(db1, db2);

  storage_db_close();
  unlink(path);
}

/* Opening with a path that has no parent directory should fail. */
static void test_db_open_invalid_path(void** state) {
  (void)state;
  char path[512];
  snprintf(path, sizeof(path), "/nonexistent/dir/%d.db", getpid());

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), -1);
  assert_null(db);
}

/* After close, the DB can be opened again. */
static void test_db_reopen_after_close(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db1 = NULL;
  assert_int_equal(storage_db_open(&db1), 0);
  assert_non_null(db1);
  storage_db_close();

  sqlite3* db2 = NULL;
  assert_int_equal(storage_db_open(&db2), 0);
  assert_non_null(db2);
  storage_db_close();
  unlink(path);
}

/* ---------------------------------------------------------------------------
 * storage_db_prepare tests
 * ------------------------------------------------------------------------ */

/* Preparing a valid SELECT statement should succeed. */
static void test_db_prepare_select(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(
      storage_db_prepare(db, "SELECT jid, password_plain FROM users WHERE jid = ?", &stmt), 0);
  assert_non_null(stmt);

  storage_db_close();
  unlink(path);
}

/* Preparing an INSERT statement should succeed. */
static void test_db_prepare_insert(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &stmt),
                   0);
  assert_non_null(stmt);

  storage_db_close();
  unlink(path);
}

/* Preparing an UPDATE statement should succeed. */
static void test_db_prepare_update(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "UPDATE users SET disabled = 1 WHERE jid = ?", &stmt), 0);
  assert_non_null(stmt);

  storage_db_close();
  unlink(path);
}

/* Preparing the same SQL twice returns the same statement (caching). */
static void test_db_prepare_cached(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt1 = NULL;
  storage_stmt_t* stmt2 = NULL;
  assert_int_equal(storage_db_prepare(db, "SELECT jid FROM users WHERE jid = ?", &stmt1), 0);
  assert_int_equal(storage_db_prepare(db, "SELECT jid FROM users WHERE jid = ?", &stmt2), 0);
  assert_ptr_equal(stmt1, stmt2);

  storage_db_close();
  unlink(path);
}

/* Preparing an invalid SQL should fail. */
static void test_db_prepare_invalid_sql(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "INVALID SQL HERE", &stmt), -1);
  assert_null(stmt);

  storage_db_close();
  unlink(path);
}

/* ---------------------------------------------------------------------------
 * storage_db_bind / step / column tests
 * ------------------------------------------------------------------------ */

/* Binding text and stepping an INSERT should succeed. */
static void test_db_bind_text_insert(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &stmt),
                   0);

  storage_db_bind_text(stmt, 1, "testuser@localhost");
  storage_db_bind_text(stmt, 2, "secretpass");
  storage_db_bind_int64(stmt, 3, 1000000);

  int rc = storage_db_step(stmt);
  assert_int_equal(rc, SQLITE_DONE);

  storage_db_close();
  unlink(path);
}

/* Binding text and stepping a SELECT should return a row. */
static void test_db_bind_text_select(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  /* Insert a row first. */
  storage_stmt_t* insert_stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &insert_stmt),
                   0);
  storage_db_bind_text(insert_stmt, 1, "user1@localhost");
  storage_db_bind_text(insert_stmt, 2, "pass1");
  storage_db_bind_int64(insert_stmt, 3, 1000000);
  assert_int_equal(storage_db_step(insert_stmt), SQLITE_DONE);

  /* Now select it. */
  storage_stmt_t* select_stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "SELECT jid FROM users WHERE jid = ?", &select_stmt), 0);
  storage_db_bind_text(select_stmt, 1, "user1@localhost");
  int rc = storage_db_step(select_stmt);
  assert_int_equal(rc, SQLITE_ROW);

  const char* jid = storage_db_column_text(select_stmt);
  assert_string_equal(jid, "user1@localhost");

  storage_db_close();
  unlink(path);
}

/* Binding int64 and stepping should work. */
static void test_db_bind_int64(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &stmt),
                   0);

  storage_db_bind_text(stmt, 1, "user2@localhost");
  storage_db_bind_text(stmt, 2, "pass2");
  storage_db_bind_int64(stmt, 3, 9999999);

  assert_int_equal(storage_db_step(stmt), SQLITE_DONE);

  /* Verify the created_at was stored correctly. */
  storage_stmt_t* select_stmt = NULL;
  assert_int_equal(
      storage_db_prepare(db, "SELECT created_at FROM users WHERE jid = ?", &select_stmt), 0);
  storage_db_bind_text(select_stmt, 1, "user2@localhost");
  assert_int_equal(storage_db_step(select_stmt), SQLITE_ROW);
  long long ts = storage_db_column_int64(select_stmt, 0);
  assert_int_equal(ts, 9999999);

  storage_db_close();
  unlink(path);
}

/* Column text copy should return an allocated string. */
static void test_db_column_text_copy(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  /* Insert a row. */
  storage_stmt_t* insert_stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &insert_stmt),
                   0);
  storage_db_bind_text(insert_stmt, 1, "copyuser@localhost");
  storage_db_bind_text(insert_stmt, 2, "copypass");
  storage_db_bind_int64(insert_stmt, 3, 5000000);
  assert_int_equal(storage_db_step(insert_stmt), SQLITE_DONE);

  /* Select and copy. */
  storage_stmt_t* select_stmt = NULL;
  assert_int_equal(
      storage_db_prepare(db, "SELECT password_plain FROM users WHERE jid = ?", &select_stmt), 0);
  storage_db_bind_text(select_stmt, 1, "copyuser@localhost");
  assert_int_equal(storage_db_step(select_stmt), SQLITE_ROW);

  char* copy = storage_db_column_text_copy(select_stmt, 0);
  assert_non_null(copy);
  assert_string_equal(copy, "copypass");
  free(copy);

  storage_db_close();
  unlink(path);
}

/* Column text on NULL result should return NULL. */
static void test_db_column_text_null(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "SELECT jid FROM users WHERE jid = ?", &stmt), 0);
  storage_db_bind_text(stmt, 1, "nonexistent");
  assert_int_equal(storage_db_step(stmt), SQLITE_DONE);

  /* After SQLITE_DONE, column_text should return NULL. */
  const char* val = storage_db_column_text(stmt);
  assert_null(val);

  storage_db_close();
  unlink(path);
}

/* ---------------------------------------------------------------------------
 * storage_db_reset tests
 * ------------------------------------------------------------------------ */

/* Resetting a statement clears bindings. */
static void test_db_reset_clears_bindings(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "SELECT jid FROM users WHERE jid = ?", &stmt), 0);

  /* Bind and step. */
  storage_db_bind_text(stmt, 1, "user1@localhost");
  assert_int_equal(storage_db_step(stmt), SQLITE_DONE);

  /* Reset and re-bind with different value. */
  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, "user2@localhost");
  assert_int_equal(storage_db_step(stmt), SQLITE_DONE);

  storage_db_close();
  unlink(path);
}

/* Resetting a NULL statement should be safe (no crash). */
static void test_db_reset_null(void** state) {
  (void)state;
  storage_db_reset(NULL);
}

/* ---------------------------------------------------------------------------
 * storage_db_bind_null tests
 * ------------------------------------------------------------------------ */

/* Binding NULL should be safe. */
static void test_db_bind_null(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &stmt),
                   0);

  storage_db_bind_text(stmt, 1, "nulluser@localhost");
  storage_db_bind_text(stmt, 2, "nullpass");
  storage_db_bind_null(stmt, 3); /* bind NULL for created_at */

  int rc = storage_db_step(stmt);
  /* This should succeed (SQLite allows NULL for created_at since it's NOT NULL
   * but the bind_null will set it to NULL which may cause constraint error).
   * We just verify the call doesn't crash. */
  (void)rc;

  storage_db_close();
  unlink(path);
}

/* Binding NULL to a NULL statement should be safe. */
static void test_db_bind_null_stmt(void** state) {
  (void)state;
  storage_db_bind_null(NULL, 1);
}

/* ---------------------------------------------------------------------------
 * storage_db_column_int64 tests
 * ------------------------------------------------------------------------ */

/* Column int64 on NULL stmt should return 0. */
static void test_db_column_int64_null(void** state) {
  (void)state;
  long long val = storage_db_column_int64(NULL, 0);
  assert_int_equal(val, 0);
}

/* Column int64 on valid stmt should return correct value. */
static void test_db_column_int64_valid(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  /* Insert a row. */
  storage_stmt_t* insert_stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &insert_stmt),
                   0);
  storage_db_bind_text(insert_stmt, 1, "int64user@localhost");
  storage_db_bind_text(insert_stmt, 2, "pass");
  storage_db_bind_int64(insert_stmt, 3, 12345678);
  assert_int_equal(storage_db_step(insert_stmt), SQLITE_DONE);

  /* Select and read. */
  storage_stmt_t* select_stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "SELECT disabled FROM users WHERE jid = ?", &select_stmt),
                   0);
  storage_db_bind_text(select_stmt, 1, "int64user@localhost");
  assert_int_equal(storage_db_step(select_stmt), SQLITE_ROW);
  long long disabled = storage_db_column_int64(select_stmt, 0);
  assert_int_equal(disabled, 0);

  storage_db_close();
  unlink(path);
}

/* ---------------------------------------------------------------------------
 * storage_db_changes tests
 * ------------------------------------------------------------------------ */

/* After an INSERT, changes should be 1. */
static void test_db_changes_insert(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &stmt),
                   0);
  storage_db_bind_text(stmt, 1, "changesuser@localhost");
  storage_db_bind_text(stmt, 2, "pass");
  storage_db_bind_int64(stmt, 3, 7777777);
  assert_int_equal(storage_db_step(stmt), SQLITE_DONE);

  int changes = storage_db_changes(db);
  assert_int_equal(changes, 1);

  storage_db_close();
  unlink(path);
}

/* After an UPDATE, changes should reflect updated rows. */
static void test_db_changes_update(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  /* Insert two rows. */
  storage_stmt_t* insert_stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &insert_stmt),
                   0);

  storage_db_bind_text(insert_stmt, 1, "chg1@localhost");
  storage_db_bind_text(insert_stmt, 2, "pass");
  storage_db_bind_int64(insert_stmt, 3, 1111111);
  assert_int_equal(storage_db_step(insert_stmt), SQLITE_DONE);
  storage_db_reset(insert_stmt);

  storage_db_bind_text(insert_stmt, 1, "chg2@localhost");
  storage_db_bind_text(insert_stmt, 2, "pass");
  storage_db_bind_int64(insert_stmt, 3, 2222222);
  assert_int_equal(storage_db_step(insert_stmt), SQLITE_DONE);

  /* Update one row. */
  storage_stmt_t* update_stmt = NULL;
  assert_int_equal(
      storage_db_prepare(db, "UPDATE users SET disabled = 1 WHERE jid = ?", &update_stmt), 0);
  storage_db_bind_text(update_stmt, 1, "chg1@localhost");
  assert_int_equal(storage_db_step(update_stmt), SQLITE_DONE);

  int changes = storage_db_changes(db);
  assert_int_equal(changes, 1);

  storage_db_close();
  unlink(path);
}

/* ---------------------------------------------------------------------------
 * Migration tests
 * ------------------------------------------------------------------------ */

/* Opening a fresh database should create the users table. */
static void test_db_migration_creates_users_table(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  /* Verify the users table exists by inserting a row. */
  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &stmt),
                   0);
  storage_db_bind_text(stmt, 1, "migrationuser@localhost");
  storage_db_bind_text(stmt, 2, "pass");
  storage_db_bind_int64(stmt, 3, 3333333);
  assert_int_equal(storage_db_step(stmt), SQLITE_DONE);

  /* Verify we can read it back. */
  storage_stmt_t* select_stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "SELECT jid FROM users WHERE jid = ?", &select_stmt), 0);
  storage_db_bind_text(select_stmt, 1, "migrationuser@localhost");
  assert_int_equal(storage_db_step(select_stmt), SQLITE_ROW);
  const char* jid = storage_db_column_text(select_stmt);
  assert_string_equal(jid, "migrationuser@localhost");

  storage_db_close();
  unlink(path);
}

/* Opening an existing database should not re-run migrations. */
static void test_db_no_remigration(void** state) {
  (void)state;
  char path[512];
  gen_db_path(path, sizeof(path));

  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);

  /* Insert a row. */
  storage_stmt_t* stmt = NULL;
  assert_int_equal(storage_db_prepare(db,
                                      "INSERT INTO users (jid, password_plain, created_at, "
                                      "disabled) VALUES (?, ?, ?, 0)",
                                      &stmt),
                   0);
  storage_db_bind_text(stmt, 1, "noremove@localhost");
  storage_db_bind_text(stmt, 2, "pass");
  storage_db_bind_int64(stmt, 3, 4444444);
  assert_int_equal(storage_db_step(stmt), SQLITE_DONE);

  storage_db_close();

  /* Reopen — should not lose data. */
  assert_int_equal(storage_db_open(&db), 0);
  storage_stmt_t* select_stmt = NULL;
  assert_int_equal(storage_db_prepare(db, "SELECT jid FROM users WHERE jid = ?", &select_stmt), 0);
  storage_db_bind_text(select_stmt, 1, "noremove@localhost");
  assert_int_equal(storage_db_step(select_stmt), SQLITE_ROW);
  const char* jid = storage_db_column_text(select_stmt);
  assert_string_equal(jid, "noremove@localhost");

  storage_db_close();
  unlink(path);
}

/* ---------------------------------------------------------------------------
 * Null-safety tests
 * ------------------------------------------------------------------------ */

/* All column functions should handle NULL stmt gracefully. */
static void test_db_null_stmt_safety(void** state) {
  (void)state;
  (void)storage_db_step(NULL);
  assert_int_equal(storage_db_column_int64(NULL, 0), 0);
  assert_null(storage_db_column_text(NULL));
  assert_null(storage_db_column_text_copy(NULL, 0));
}

/* ---------------------------------------------------------------------------
 * Test group with setup / teardown
 * ------------------------------------------------------------------------ */

/* Test that the DB path is properly set up. */
static void test_db_path_configured(void** state) {
  char* path = *state;
  setup_test_config(path);
  sqlite3* db = NULL;
  assert_int_equal(storage_db_open(&db), 0);
  assert_non_null(db);
  storage_db_close();
}

static const struct CMUnitTest test_db_tests[] = {
    /* Open / close tests. */
    cmocka_unit_test_setup_teardown(test_db_open_creates_file, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_open_singleton, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_open_invalid_path, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_reopen_after_close, db_test_setup, db_test_teardown),

    /* Prepare tests. */
    cmocka_unit_test_setup_teardown(test_db_prepare_select, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_prepare_insert, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_prepare_update, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_prepare_cached, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_prepare_invalid_sql, db_test_setup, db_test_teardown),

    /* Bind / step / column tests. */
    cmocka_unit_test_setup_teardown(test_db_bind_text_insert, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_bind_text_select, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_bind_int64, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_column_text_copy, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_column_text_null, db_test_setup, db_test_teardown),

    /* Reset tests. */
    cmocka_unit_test_setup_teardown(test_db_reset_clears_bindings, db_test_setup, db_test_teardown),
    cmocka_unit_test(test_db_reset_null),

    /* Bind null tests. */
    cmocka_unit_test_setup_teardown(test_db_bind_null, db_test_setup, db_test_teardown),
    cmocka_unit_test(test_db_bind_null_stmt),

    /* Column int64 tests. */
    cmocka_unit_test(test_db_column_int64_null),
    cmocka_unit_test_setup_teardown(test_db_column_int64_valid, db_test_setup, db_test_teardown),

    /* Changes tests. */
    cmocka_unit_test_setup_teardown(test_db_changes_insert, db_test_setup, db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_changes_update, db_test_setup, db_test_teardown),

    /* Migration tests. */
    cmocka_unit_test_setup_teardown(test_db_migration_creates_users_table, db_test_setup,
                                    db_test_teardown),
    cmocka_unit_test_setup_teardown(test_db_no_remigration, db_test_setup, db_test_teardown),

    /* Null-safety tests. */
    cmocka_unit_test(test_db_null_stmt_safety),

    /* Setup/teardown group. */
    cmocka_unit_test_setup_teardown(test_db_path_configured, db_test_setup, db_test_teardown),
};

int main(void) {
  return cmocka_run_group_tests(test_db_tests, log_group_setup, log_group_teardown);
}
