#include "storage/roster.h"
#include "storage/db.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

/* Static storage for groups returned by storage_roster_get_groups. */
#define MAX_GROUPS_STATIC 32
static char s_group_bufs[MAX_GROUPS_STATIC][256];

int storage_roster_list(const char* owner_jid, storage_roster_item_cb cb, void* ud) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !cb) return -1;
  if (storage_db_open(&db) != 0) return -1;

  rc = storage_db_prepare(db,
                          "SELECT contact_jid, name, subscription, ask"
                          " FROM roster WHERE owner_jid = ?",
                          &stmt);
  if (rc != 0) return -1;

  storage_db_bind_text(stmt, 1, owner_jid);

  while ((rc = storage_db_step(stmt)) == SQLITE_ROW) {
    storage_roster_item_t item;
    memset(&item, 0, sizeof(item));

    char* cjid = storage_db_column_text_copy(stmt, 0);
    char* name = storage_db_column_text_copy(stmt, 1);
    char* sub  = storage_db_column_text_copy(stmt, 2);
    long long ask = storage_db_column_int64(stmt, 3);

    if (cjid) { strncpy(item.contact_jid, cjid, sizeof(item.contact_jid) - 1); free(cjid); }
    if (name) { strncpy(item.name, name, sizeof(item.name) - 1); free(name); }
    if (sub)  { strncpy(item.subscription, sub, sizeof(item.subscription) - 1); free(sub); }
    item.ask = (int)ask;

    const char* groups[MAX_GROUPS_STATIC];
    int gc = storage_roster_get_groups(owner_jid, item.contact_jid, groups, MAX_GROUPS_STATIC);
    if (gc < 0) gc = 0;

    if (cb(&item, groups, gc, ud) != 0) break;
  }

  storage_db_reset(stmt);

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    stump_er("roster list error for '%s': %s", owner_jid, sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}

int storage_roster_get(const char* owner_jid, const char* contact_jid,
                       storage_roster_item_t* item_out) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !contact_jid || !item_out) return -1;
  if (storage_db_open(&db) != 0) return -1;

  rc = storage_db_prepare(db,
                          "SELECT contact_jid, name, subscription, ask"
                          " FROM roster WHERE owner_jid = ? AND contact_jid = ?",
                          &stmt);
  if (rc != 0) return -1;

  storage_db_bind_text(stmt, 1, owner_jid);
  storage_db_bind_text(stmt, 2, contact_jid);

  rc = storage_db_step(stmt);
  if (rc == SQLITE_ROW) {
    memset(item_out, 0, sizeof(*item_out));
    char* cjid = storage_db_column_text_copy(stmt, 0);
    char* name = storage_db_column_text_copy(stmt, 1);
    char* sub  = storage_db_column_text_copy(stmt, 2);
    long long ask = storage_db_column_int64(stmt, 3);

    if (cjid) { strncpy(item_out->contact_jid, cjid, sizeof(item_out->contact_jid) - 1); free(cjid); }
    if (name) { strncpy(item_out->name, name, sizeof(item_out->name) - 1); free(name); }
    if (sub)  { strncpy(item_out->subscription, sub, sizeof(item_out->subscription) - 1); free(sub); }
    item_out->ask = (int)ask;

    storage_db_reset(stmt);
    return 0;
  }

  storage_db_reset(stmt);
  return (rc == SQLITE_DONE) ? 1 : -1;
}

int storage_roster_upsert(const char* owner_jid, const storage_roster_item_t* item,
                          const char** groups, int group_count) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !item) return -1;
  if (storage_db_open(&db) != 0) return -1;

  /* Upsert the main row. */
  rc = storage_db_prepare(db,
                          "INSERT INTO roster (owner_jid, contact_jid, name, subscription, ask)"
                          " VALUES (?, ?, ?, ?, ?)"
                          " ON CONFLICT(owner_jid, contact_jid) DO UPDATE SET"
                          "   name = excluded.name,"
                          "   subscription = excluded.subscription,"
                          "   ask = excluded.ask",
                          &stmt);
  if (rc != 0) return -1;

  storage_db_bind_text(stmt, 1, owner_jid);
  storage_db_bind_text(stmt, 2, item->contact_jid);
  storage_db_bind_text(stmt, 3, item->name);
  storage_db_bind_text(stmt, 4, item->subscription[0] ? item->subscription : "none");
  storage_db_bind_int64(stmt, 5, (long long)item->ask);

  rc = storage_db_step(stmt);
  storage_db_reset(stmt);
  if (rc != SQLITE_DONE) {
    stump_er("roster upsert failed for '%s'/'%s': %s", owner_jid, item->contact_jid,
             sqlite3_errmsg(db));
    return -1;
  }

  /* Replace groups: delete old, insert new. */
  storage_stmt_t* del_stmt = NULL;
  rc = storage_db_prepare(db,
                          "DELETE FROM roster_groups WHERE owner_jid = ? AND contact_jid = ?",
                          &del_stmt);
  if (rc != 0) return -1;
  storage_db_bind_text(del_stmt, 1, owner_jid);
  storage_db_bind_text(del_stmt, 2, item->contact_jid);
  rc = storage_db_step(del_stmt);
  storage_db_reset(del_stmt);
  if (rc != SQLITE_DONE) return -1;

  if (groups && group_count > 0) {
    storage_stmt_t* ins_stmt = NULL;
    rc = storage_db_prepare(db,
                            "INSERT OR IGNORE INTO roster_groups (owner_jid, contact_jid, group_name)"
                            " VALUES (?, ?, ?)",
                            &ins_stmt);
    if (rc != 0) return -1;
    for (int i = 0; i < group_count; i++) {
      if (!groups[i] || groups[i][0] == '\0') continue;
      storage_db_bind_text(ins_stmt, 1, owner_jid);
      storage_db_bind_text(ins_stmt, 2, item->contact_jid);
      storage_db_bind_text(ins_stmt, 3, groups[i]);
      rc = storage_db_step(ins_stmt);
      storage_db_reset(ins_stmt);
      if (rc != SQLITE_DONE) return -1;
    }
  }

  return 0;
}

int storage_roster_remove(const char* owner_jid, const char* contact_jid) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!owner_jid || !contact_jid) return -1;
  if (storage_db_open(&db) != 0) return -1;

  rc = storage_db_prepare(db,
                          "DELETE FROM roster WHERE owner_jid = ? AND contact_jid = ?",
                          &stmt);
  if (rc != 0) return -1;

  storage_db_bind_text(stmt, 1, owner_jid);
  storage_db_bind_text(stmt, 2, contact_jid);
  rc = storage_db_step(stmt);
  storage_db_reset(stmt);

  if (rc != SQLITE_DONE) {
    stump_er("roster remove failed for '%s'/'%s': %s", owner_jid, contact_jid,
             sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}

int storage_roster_get_groups(const char* owner_jid, const char* contact_jid,
                               const char** groups_out, int max_groups) {
  sqlite3* db;
  storage_stmt_t* stmt = NULL;
  int rc;
  int count = 0;

  if (!owner_jid || !contact_jid || !groups_out || max_groups <= 0) return -1;
  if (storage_db_open(&db) != 0) return -1;

  rc = storage_db_prepare(db,
                          "SELECT group_name FROM roster_groups"
                          " WHERE owner_jid = ? AND contact_jid = ?",
                          &stmt);
  if (rc != 0) return -1;

  storage_db_bind_text(stmt, 1, owner_jid);
  storage_db_bind_text(stmt, 2, contact_jid);

  while ((rc = storage_db_step(stmt)) == SQLITE_ROW && count < max_groups) {
    char* g = storage_db_column_text_copy(stmt, 0);
    if (g) {
      strncpy(s_group_bufs[count], g, sizeof(s_group_bufs[count]) - 1);
      s_group_bufs[count][sizeof(s_group_bufs[count]) - 1] = '\0';
      groups_out[count] = s_group_bufs[count];
      free(g);
      count++;
    }
  }

  storage_db_reset(stmt);
  return count;
}
