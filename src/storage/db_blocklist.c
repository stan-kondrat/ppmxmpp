#include "storage/db_blocklist.h"
#include "storage/db.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

int storage_blocklist_check(const char* owner_jid, const char* blocked_jid) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !blocked_jid) {
    stump_er("blocklist check: invalid arguments");
    return -1;
  }
  if (storage_db_open(&db) != 0) {
    stump_er("blocklist check: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db,
                          "SELECT 1 FROM blocklist WHERE owner_jid = ? AND blocked_jid = ?",
                          &stmt);
  if (rc != 0) {
    stump_er("blocklist check: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, owner_jid);
  storage_db_bind_text(stmt, 2, blocked_jid);

  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();

  if (rc == SQLITE_ROW) {
    return 1;
  }
  if (rc == SQLITE_DONE) {
    return 0;
  }
  stump_er("blocklist check error for '%s'/'%s': %s", owner_jid, blocked_jid, sqlite3_errmsg(db));
  return -1;
}

int storage_blocklist_add(const char* owner_jid, const char* blocked_jid) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !blocked_jid) {
    stump_er("blocklist add: invalid arguments");
    return -1;
  }
  if (storage_db_open(&db) != 0) {
    stump_er("blocklist add: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db,
                          "INSERT OR IGNORE INTO blocklist (owner_jid, blocked_jid)"
                          " VALUES (?, ?)",
                          &stmt);
  if (rc != 0) {
    stump_er("blocklist add: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, owner_jid);
  storage_db_bind_text(stmt, 2, blocked_jid);

  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();

  if (rc != SQLITE_DONE) {
    stump_er("blocklist add failed for '%s'/'%s': %s", owner_jid, blocked_jid,
             sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}

int storage_blocklist_remove(const char* owner_jid, const char* blocked_jid) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !blocked_jid) {
    stump_er("blocklist remove: invalid arguments");
    return -1;
  }
  if (storage_db_open(&db) != 0) {
    stump_er("blocklist remove: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db,
                          "DELETE FROM blocklist WHERE owner_jid = ? AND blocked_jid = ?",
                          &stmt);
  if (rc != 0) {
    stump_er("blocklist remove: prepare failed");
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, owner_jid);
  storage_db_bind_text(stmt, 2, blocked_jid);

  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  storage_db_close();

  if (rc != SQLITE_DONE) {
    stump_er("blocklist remove failed for '%s'/'%s': %s", owner_jid, blocked_jid,
             sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}

int storage_blocklist_list(const char* owner_jid, storage_blocklist_cb cb, void* ud) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !cb) {
    stump_er("blocklist list: invalid arguments");
    return -1;
  }
  if (storage_db_open(&db) != 0) {
    stump_er("blocklist list: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db,
                          "SELECT blocked_jid FROM blocklist WHERE owner_jid = ?",
                          &stmt);
  if (rc != 0) {
    stump_er("blocklist list: prepare failed for '%s'", owner_jid);
    storage_db_close();
    return -1;
  }

  storage_db_bind_text(stmt, 1, owner_jid);

  while ((rc = storage_db_step(stmt)) == SQLITE_ROW) {
    char* bj = storage_db_column_text_copy(stmt, 0);
    if (bj) {
      if (cb(bj, ud) != 0) {
        free(bj);
        break;
      }
      free(bj);
    }
  }

  storage_db_reset(stmt);
  storage_db_close();

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    stump_er("blocklist list error for '%s': %s", owner_jid, sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}