#include "storage/db_users.h"
#include "storage/db.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

/* Crypto helpers from xmpp_sasl_scram.c — shared with storage layer. */
extern int scram_hi(const char* password, const uint8_t* salt, size_t salt_len,
                   int iterations, uint8_t output[32]);
extern void scram_derive_stored_key(const uint8_t* salted_password, uint8_t output[32]);
extern void scram_derive_server_key(const uint8_t* salted_password, uint8_t output[32]);
extern char* scram_saslprep(const char* password, size_t password_len);

/* Static storage for user data returned by get_by_jid. */
static char static_jid[512];
static char static_password[512];

int storage_users_get_by_jid(const char* bare_jid, storage_user_t* user_out) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!bare_jid || !user_out) {
    stump_er("users get_by_jid: invalid arguments");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("users get_by_jid: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(
      db, "SELECT jid, password_plain FROM users WHERE jid = ? AND disabled = 0", &stmt);
  if (rc != 0) {
    stump_er("users get_by_jid: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, bare_jid);
  rc = storage_db_step(stmt);

  if (rc == SQLITE_ROW) {
    char* jid_copy = storage_db_column_text_copy(stmt, 0);
    char* pass_copy = storage_db_column_text_copy(stmt, 1);

    if (jid_copy && pass_copy) {
      strncpy(static_jid, jid_copy, sizeof(static_jid) - 1);
      static_jid[sizeof(static_jid) - 1] = '\0';
      strncpy(static_password, pass_copy, sizeof(static_password) - 1);
      static_password[sizeof(static_password) - 1] = '\0';

      user_out->jid = static_jid;
      user_out->password_plain = static_password;
      free(jid_copy);
      free(pass_copy);
      storage_db_reset(stmt);
      storage_db_close();
      return 0;
    }

    free(jid_copy);
    free(pass_copy);
  }

  storage_db_reset(stmt);
  storage_db_close();
  return 1; /* not found */
}

int storage_users_check_password(const char* bare_jid, const char* password) {
  if (!password) {
    return 0;
  }

  storage_user_t user;
  int rc = storage_users_get_by_jid(bare_jid, &user);

  if (rc != 0) {
    return 0; /* not found or error */
  }

  if (strcmp(user.password_plain, password) == 0) {
    return 1;
  }

  return 0;
}

int storage_users_create(const char* bare_jid, const char* password_plain) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;
  long long now;

  if (!bare_jid || !password_plain) {
    stump_er("users create: invalid arguments");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("users create: cannot open database");
    return -1;
  }

  now = (long long)time(NULL);

  rc = storage_db_prepare(db,
                          "INSERT INTO users (jid, password_plain, created_at, "
                          "disabled) VALUES (?, ?, ?, 0)",
                          &stmt);
  if (rc != 0) {
    stump_er("users create: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, bare_jid);
  storage_db_bind_text(stmt, 2, password_plain);
  storage_db_bind_int64(stmt, 3, now);

  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();

  if (rc != SQLITE_DONE) {
    if (rc == SQLITE_CONSTRAINT) {
      stump_w("user '%s' already exists", bare_jid);
      return -1;
    }
    stump_er("failed to create user '%s': %s", bare_jid, sqlite3_errmsg(db));
    return -1;
  }

  stump_i("created user: %s", bare_jid);
  return 0;
}

int storage_users_disable(const char* bare_jid) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (storage_db_open(&db) != 0) {
    stump_er("users disable: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db, "UPDATE users SET disabled = 1 WHERE jid = ?", &stmt);
  if (rc != 0) {
    stump_er("users disable: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, bare_jid);
  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();

  if (rc != SQLITE_DONE) {
    stump_er("failed to disable user '%s': %s", bare_jid, sqlite3_errmsg(db));
    return -1;
  }

  if (storage_db_changes(db) == 0) {
    stump_er("users disable: no rows affected for '%s'", bare_jid);
    return -1;
  }

  stump_i("disabled user: %s", bare_jid);
  return 0;
}

int storage_users_delete(const char* bare_jid) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!bare_jid) {
    stump_er("users delete: NULL bare_jid");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("users delete: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db, "DELETE FROM users WHERE jid = ?", &stmt);
  if (rc != 0) {
    stump_er("users delete: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, bare_jid);
  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();

  if (rc == SQLITE_DONE) {
    return 0;  /* deleted or didn't exist */
  }
  stump_er("failed to delete user '%s'", bare_jid);
  return -1;
}

/* ------------------------------------------------------------------ */
/*  SCRAM credential management                                        */
/* ------------------------------------------------------------------ */

int storage_scram_get_by_jid(const char* bare_jid,
                             uint8_t stored_key[32],
                             uint8_t server_key[32],
                             uint8_t salt[64],
                             size_t* salt_len_out,
                             int* iter_out) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!bare_jid || !stored_key || !server_key || !salt || !salt_len_out || !iter_out) {
    stump_er("scram get_by_jid: invalid arguments");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("scram get_by_jid: cannot open database");
    return -1;
  }

  /* Query: returns salt_base64, stored_key, server_key, iteration_count.
   * The base64 columns are decoded below. */
  rc = storage_db_prepare(db,
                          "SELECT scram_salt_base64, scram_stored_key_base64, "
                          "scram_server_key_base64, scram_iteration_count "
                          "FROM users WHERE jid = ? AND scram_stored_key_base64 IS NOT NULL",
                          &stmt);
  if (rc != 0) {
    stump_er("scram get_by_jid: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, bare_jid);
  rc = storage_db_step(stmt);

  if (rc != SQLITE_ROW) {
    storage_db_reset(stmt);
    storage_db_close();
    return 1; /* not found / no SCRAM credentials */
  }

  /* Decode base64 fields. */
  char* salt_b64 = storage_db_column_text_copy(stmt, 0);
  char* stored_key_b64 = storage_db_column_text_copy(stmt, 1);
  char* server_key_b64 = storage_db_column_text_copy(stmt, 2);
  *iter_out = (int)storage_db_column_int64(stmt, 3);
  storage_db_reset(stmt);
  storage_db_close();

  if (!salt_b64 || !stored_key_b64 || !server_key_b64) {
    free(salt_b64);
    free(stored_key_b64);
    free(server_key_b64);
    return 1;
  }

  extern int scram_base64_decode(const char* input, size_t in_len,
                                 uint8_t* output, size_t out_cap);

  *salt_len_out = (size_t)scram_base64_decode(salt_b64, strlen(salt_b64),
                                              salt, 64);
  if (*salt_len_out == (size_t)-1) {
    free(salt_b64);
    free(stored_key_b64);
    free(server_key_b64);
    return -1;
  }

  if (scram_base64_decode(stored_key_b64, strlen(stored_key_b64),
                          stored_key, 32) != 32 ||
      scram_base64_decode(server_key_b64, strlen(server_key_b64),
                          server_key, 32) != 32) {
    free(salt_b64);
    free(stored_key_b64);
    free(server_key_b64);
    return -1;
  }

  free(salt_b64);
  free(stored_key_b64);
  free(server_key_b64);
  return 0;
}

int storage_scram_has_scram_credentials(const char* bare_jid) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;


  if (!bare_jid) {
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    return -1;
  }

  rc = storage_db_prepare(db,
                          "SELECT 1 FROM users WHERE jid = ? "
                          "AND scram_stored_key_base64 IS NOT NULL LIMIT 1",
                          &stmt);
  if (rc != 0) {
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, bare_jid);
  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();

  return (rc == SQLITE_ROW) ? 1 : 0;
}

int storage_scram_set_password(const char* bare_jid, const char* password,
                                const uint8_t* salt, size_t salt_len, int iterations) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;


  if (!bare_jid || !password || !salt || salt_len == 0 || iterations < 1) {
    stump_er("scram set_password: invalid arguments");
    return -1;
  }

  /* SASLprep normalization. */
  char* normalized = scram_saslprep(password, strlen(password));
  if (!normalized) {
    stump_er("scram set_password: SASLprep normalization failed for '%s'", bare_jid);
    return -1;
  }

  /* Compute SaltedPassword = Hi(Normalize(password), salt, i). */
  uint8_t salted_password[32];
  if (scram_hi(normalized, salt, salt_len, iterations, salted_password) != 0) {
    free(normalized);
    return -1;
  }

  /* Derive StoredKey = H(ClientKey) = H(HMAC(SaltedPassword, "Client Key")). */
  uint8_t stored_key[32];
  scram_derive_stored_key(salted_password, stored_key);

  /* Derive ServerKey = HMAC(SaltedPassword, "Server Key"). */
  uint8_t server_key[32];
  scram_derive_server_key(salted_password, server_key);

  free(normalized);
  normalized = NULL;

  /* Base64 encode the binary values for storage. */
  extern int scram_base64_encode(const uint8_t* input, size_t in_len,
                                 char* output, size_t out_cap);

  char salt_b64[128];
  char stored_key_b64[128];
  char server_key_b64[128];

  if (scram_base64_encode(salt, salt_len, salt_b64, sizeof(salt_b64)) < 0 ||
      scram_base64_encode(stored_key, 32, stored_key_b64, sizeof(stored_key_b64)) < 0 ||
      scram_base64_encode(server_key, 32, server_key_b64, sizeof(server_key_b64)) < 0) {
    stump_er("scram set_password: base64 encoding overflow");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("scram set_password: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db,
                          "UPDATE users SET "
                          "scram_salt_base64 = ?,"
                          "scram_stored_key_base64 = ?,"
                          "scram_server_key_base64 = ?,"
                          "scram_iteration_count = ? "
                          "WHERE jid = ?",
                          &stmt);
  if (rc != 0) {
    stump_er("scram set_password: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, salt_b64);
  storage_db_bind_text(stmt, 2, stored_key_b64);
  storage_db_bind_text(stmt, 3, server_key_b64);
  storage_db_bind_int64(stmt, 4, (long long)iterations);
  storage_db_bind_text(stmt, 5, bare_jid);

  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();


  if (rc != SQLITE_DONE) {
    stump_er("scram set_password: failed for '%s': %s", bare_jid, sqlite3_errmsg(db));
    return -1;
  }

  stump_i("scram set_password: stored credentials for '%s' (iter=%d)", bare_jid, iterations);
  return 0;
}
