#include "storage/db_vcard.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "storage/db.h"

/* ------------------------------------------------------------------ */
/*  vCard storage — stores the raw XML of the <vCard> subtree        */
/* ------------------------------------------------------------------ */

int storage_vcard_get(const char* bare_jid, char** xml_out) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!bare_jid || !xml_out) {
    stump_er("storage_vcard_get: NULL argument");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("storage_vcard_get: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db, "SELECT vcard_xml FROM vcards WHERE jid = ?", &stmt);
  if (rc != 0) {
    stump_er("storage_vcard_get: cannot prepare statement");
    storage_db_close();
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, bare_jid);

  rc = storage_db_step(stmt);
  if (rc == SQLITE_ROW) {
    char* copy = storage_db_column_text_copy(stmt, 0);
    storage_db_close();
    if (!copy) {
      stump_er("storage_vcard_get: NULL vcard_xml in database for %s", bare_jid);
      return -1;
    }
    *xml_out = copy;
    return 0;
  }

  if (rc != SQLITE_DONE) {
    stump_er("storage_vcard_get: step failed: %s", sqlite3_errmsg(db));
    storage_db_close();
    return -1;
  }

  storage_db_close();
  return 1;  /* not found */
}

int storage_vcard_set(const char* bare_jid, const char* vcard_xml) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!bare_jid || !vcard_xml) {
    stump_er("storage_vcard_set: NULL argument");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("storage_vcard_set: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db,
                          "INSERT INTO vcards (jid, vcard_xml) VALUES (?, ?) "
                          "ON CONFLICT(jid) DO UPDATE SET vcard_xml = excluded.vcard_xml",
                          &stmt);
  if (rc != 0) {
    stump_er("storage_vcard_set: cannot prepare statement");
    storage_db_close();
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, bare_jid);
  storage_db_bind_text(stmt, 2, vcard_xml);

  rc = storage_db_step(stmt);
  if (rc != SQLITE_DONE) {
    stump_er("storage_vcard_set: insert/update failed: %s", sqlite3_errmsg(db));
    storage_db_close();
    return -1;
  }

  stump_d("storage_vcard_set: stored vCard for %s", bare_jid);
  storage_db_close();
  return 0;
}

int storage_vcard_delete(const char* bare_jid) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!bare_jid) {
    stump_er("storage_vcard_delete: NULL bare_jid");
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("storage_vcard_delete: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db, "DELETE FROM vcards WHERE jid = ?", &stmt);
  if (rc != 0) {
    stump_er("storage_vcard_delete: cannot prepare statement");
    storage_db_close();
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, bare_jid);

  rc = storage_db_step(stmt);
  if (rc != SQLITE_DONE) {
    stump_er("storage_vcard_delete: delete failed: %s", sqlite3_errmsg(db));
    storage_db_close();
    return -1;
  }

  storage_db_close();
  return 0;
}