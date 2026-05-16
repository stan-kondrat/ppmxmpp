#include "storage/db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "log.h"

/* Maximum number of cached statements. */
#define MAX_STMTS 64

/* Internal DB state. All SQLite calls run on the libuv main thread. */
static sqlite3* g_db = NULL;
static storage_stmt_t g_stmts[MAX_STMTS];
static int g_stmt_count = 0;

/* Migration SQL for each version. Each entry is applied inside a transaction.
 */
static const char* MIGRATIONS[] = {
    /* Version 1: create users table */
    "CREATE TABLE IF NOT EXISTS users (\n"
    "    jid           TEXT PRIMARY KEY,\n"
    "    password_plain TEXT NOT NULL,\n"
    "    created_at    INTEGER NOT NULL,\n"
    "    disabled      INTEGER NOT NULL DEFAULT 0\n"
    ")\n",

    /* Version 2: roster tables (RFC 6121 §2) */
    "CREATE TABLE IF NOT EXISTS roster (\n"
    "    owner_jid     TEXT NOT NULL,\n"
    "    contact_jid   TEXT NOT NULL,\n"
    "    name          TEXT NOT NULL DEFAULT '',\n"
    "    subscription  TEXT NOT NULL DEFAULT 'none',\n"
    "    ask           INTEGER NOT NULL DEFAULT 0,\n"
    "    PRIMARY KEY (owner_jid, contact_jid)\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS roster_groups (\n"
    "    owner_jid     TEXT NOT NULL,\n"
    "    contact_jid   TEXT NOT NULL,\n"
    "    group_name    TEXT NOT NULL,\n"
    "    PRIMARY KEY (owner_jid, contact_jid, group_name),\n"
    "    FOREIGN KEY (owner_jid, contact_jid)\n"
    "        REFERENCES roster(owner_jid, contact_jid) ON DELETE CASCADE\n"
    ")\n",

    /* Version 3: offline messages (XEP-0160, XEP-0203) */
    "CREATE TABLE IF NOT EXISTS offline_messages (\n"
    "    id              INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    recipient_jid  TEXT NOT NULL,\n"
    "    sender_jid     TEXT NOT NULL,\n"
    "    stanza_xml     TEXT NOT NULL,\n"
    "    received_at    INTEGER NOT NULL,\n"
    "    bytes_size     INTEGER NOT NULL DEFAULT 0\n"
    ")\n",
    "CREATE INDEX IF NOT EXISTS idx_offline_recipient ON offline_messages(recipient_jid, received_at)\n",
};

static int ensure_directory(const char* path) {
  char* dir = strdup(path);
  if (!dir) {
    stump_er("cannot allocate memory for directory path");
    return -1;
  }
  char* slash = strrchr(dir, '/');
  if (!slash) {
    free(dir);
    return 0;
  }
  *slash = '\0';
  int rc = 0;
  if (mkdir(dir, S_IRWXU | S_IRWXG) != 0 && errno != EEXIST) {
    stump_er("cannot create database directory '%s': %s", dir, strerror(errno));
    rc = -1;
  }
  free(dir);
  return rc;
}

static int apply_migrations(sqlite3* db) {
  sqlite3_stmt* ver_stmt = NULL;
  long long current_version = 0;
  int rc;

  /* Create schema_version table if it doesn't exist. */
  rc = sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER PRIMARY KEY)",
                    NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    stump_er("failed to create schema_version table: %s", sqlite3_errmsg(db));
    return -1;
  }

  /* Read current version. */
  rc = sqlite3_prepare_v2(db, "SELECT version FROM schema_version LIMIT 1", -1, &ver_stmt, NULL);
  if (rc != SQLITE_OK) {
    stump_er("failed to prepare version query: %s", sqlite3_errmsg(db));
    return -1;
  }

  if (sqlite3_step(ver_stmt) == SQLITE_ROW) {
    current_version = sqlite3_column_int64(ver_stmt, 0);
  }
  sqlite3_finalize(ver_stmt);
  ver_stmt = NULL;

  if (current_version >= STORAGE_MAX_VERSION) {
    stump_i("database schema is up to date (version %d)", (int)current_version);
    return 0;
  }

  /* Run migrations in a transaction. */
  rc = sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    stump_er("failed to begin migration transaction: %s", sqlite3_errmsg(db));
    return -1;
  }

  for (long long v = current_version + 1; v <= STORAGE_MAX_VERSION; v++) {
    stump_i("applying migration version %lld", v);
    rc = sqlite3_exec(db, MIGRATIONS[(size_t)v - 1], NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      stump_er("migration version %lld failed: %s", v, sqlite3_errmsg(db));
      return -1;
    }
  }

  {
    sqlite3_stmt* ver_update = NULL;
    rc = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO schema_version (version) VALUES (?)", -1,
                            &ver_update, NULL);
    if (rc != SQLITE_OK) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      stump_er("failed to prepare schema version update: %s", sqlite3_errmsg(db));
      return -1;
    }
    sqlite3_bind_int64(ver_update, 1, STORAGE_MAX_VERSION);
    rc = sqlite3_step(ver_update);
    sqlite3_finalize(ver_update);
    ver_update = NULL;
    if (rc != SQLITE_DONE) {
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      stump_er("failed to update schema version: %s", sqlite3_errmsg(db));
      return -1;
    }
  }

  rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    stump_er("failed to commit migration transaction: %s", sqlite3_errmsg(db));
    return -1;
  }

  stump_i("database schema updated to version %d", STORAGE_MAX_VERSION);
  return 0;
}

int storage_db_open(sqlite3** db_out) {
  int rc;
  struct stat st;

  if (g_db) {
    if (db_out) {
      *db_out = g_db;
    }
    return 0;
  }

  const char* db_path = server_config.db_path;

  if (ensure_directory(db_path) != 0) {
    stump_er("storage_db_open: cannot ensure directory");
    return -1;
  }

  if (stat(db_path, &st) == 0) {
    stump_i("database found: %s", db_path);
  } else {
    stump_i("database not found, will be created: %s", db_path);
  }

  rc = sqlite3_open(db_path, &g_db);
  if (rc != SQLITE_OK) {
    stump_er("cannot open database '%s': %s", db_path, sqlite3_errmsg(g_db));
    return -1;
  }

  /* WAL mode for better concurrency. */
  rc = sqlite3_exec(g_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    stump_er("failed to set WAL mode: %s", sqlite3_errmsg(g_db));
  }

  /* Apply migrations. */
  if (apply_migrations(g_db) != 0) {
    stump_er("storage_db_open: apply_migrations failed");
    sqlite3_close(g_db);
    g_db = NULL;
    return -1;
  }

  *db_out = g_db;
  return 0;
}

void storage_db_close(void) {
  /* Free all cached statements. */
  for (int i = 0; i < g_stmt_count; i++) {
    if (g_stmts[i].stmt) {
      sqlite3_finalize(g_stmts[i].stmt);
      g_stmts[i].stmt = NULL;
    }
  }
  g_stmt_count = 0;

  if (g_db) {
    sqlite3_close(g_db);
    g_db = NULL;
  }
}

int storage_db_prepare(sqlite3* db, const char* sql, storage_stmt_t** stmt_out) {
  /* Check if statement is already cached. */
  for (int i = 0; i < g_stmt_count; i++) {
    if (strcmp(g_stmts[i].sql, sql) == 0) {
      *stmt_out = &g_stmts[i];
      return 0;
    }
  }

  /* Cache is full. */
  if (g_stmt_count >= MAX_STMTS) {
    stump_er("statement cache full (%d)", MAX_STMTS);
    return -1;
  }

  storage_stmt_t* stmt = &g_stmts[g_stmt_count];
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt->stmt, NULL);
  if (rc != SQLITE_OK) {
    stump_er("failed to prepare statement: %s", sqlite3_errmsg(db));
    return -1;
  }

  strncpy(stmt->sql, sql, sizeof(stmt->sql) - 1);
  stmt->sql[sizeof(stmt->sql) - 1] = '\0';
  g_stmt_count++;

  *stmt_out = stmt;
  return 0;
}

void storage_db_reset(storage_stmt_t* stmt) {
  if (stmt && stmt->stmt) {
    sqlite3_reset(stmt->stmt);
    sqlite3_clear_bindings(stmt->stmt);
  }
}

void storage_db_bind_null(storage_stmt_t* stmt, int idx) {
  if (stmt && stmt->stmt) {
    sqlite3_bind_null(stmt->stmt, idx);
  }
}

void storage_db_bind_int64(storage_stmt_t* stmt, int idx, long long value) {
  if (stmt && stmt->stmt) {
    sqlite3_bind_int64(stmt->stmt, idx, value);
  }
}

void storage_db_bind_text(storage_stmt_t* stmt, int idx, const char* value) {
  if (stmt && stmt->stmt && value) {
    sqlite3_bind_text(stmt->stmt, idx, value, -1, SQLITE_TRANSIENT);
  }
}

int storage_db_step(storage_stmt_t* stmt) {
  if (!stmt || !stmt->stmt) {
    return SQLITE_ERROR;
  }
  return sqlite3_step(stmt->stmt);
}

long long storage_db_column_int64(storage_stmt_t* stmt, int col) {
  if (!stmt || !stmt->stmt) {
    return 0;
  }
  return sqlite3_column_int64(stmt->stmt, col);
}

const char* storage_db_column_text(storage_stmt_t* stmt) {
  if (!stmt || !stmt->stmt) {
    return NULL;
  }
  const unsigned char* col = sqlite3_column_text(stmt->stmt, 0);
  return col ? (const char*)col : NULL;
}

const char* storage_db_column_text_col(storage_stmt_t* stmt, int col) {
  if (!stmt || !stmt->stmt) {
    return NULL;
  }
  const unsigned char* val = sqlite3_column_text(stmt->stmt, col);
  return val ? (const char*)val : NULL;
}

char* storage_db_column_text_copy(storage_stmt_t* stmt, int col) {
  if (!stmt || !stmt->stmt) {
    return NULL;
  }
  const unsigned char* data = sqlite3_column_text(stmt->stmt, col);
  if (!data) {
    return NULL;
  }
  return strdup((const char*)data);
}

int storage_db_changes(sqlite3* db) { return sqlite3_changes(db); }
