#include "storage/db_offline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "stumpless.h"
#include "storage/db.h"

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Get current Unix timestamp in seconds. */
static long long now_seconds(void) {
  return (long long)time(NULL);
}

/* Format Unix timestamp as XEP-0203 datetime string (UTC).
 * Output format: YYYY-MM-DDTHH:MM:SSZ (ISO 8601 / XEP-0082)
 * Returns 0 on success, -1 on error. */
static int format_datetime(char* out, size_t out_size, long long unix_time) {
  struct tm tm_buf;
  gmtime_r((const time_t*)&unix_time, &tm_buf);
  size_t rc = strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
  if (rc == 0) {
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int offline_store(const char* recipient_bare_jid, const char* sender_jid, const char* stanza_xml,
                  size_t stanza_len) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!recipient_bare_jid || !sender_jid || !stanza_xml) {
    stump_er("offline_store: NULL argument");
    return -1;
  }

  if (offline_is_capped(recipient_bare_jid)) {
    stump_d("offline_store: recipient '%s' has reached storage cap", recipient_bare_jid);
    return -2;
  }

  if (storage_db_open(&db) != 0) {
    stump_er("offline_store: cannot open database");
    return -1;
  }

  rc = storage_db_prepare(db,
                          "INSERT INTO offline_messages (recipient_jid, sender_jid, stanza_xml, "
                          "received_at, bytes_size) VALUES (?, ?, ?, ?, ?)",
                          &stmt);
  if (rc != 0) {
    stump_er("offline_store: cannot prepare statement");
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, recipient_bare_jid);
  storage_db_bind_text(stmt, 2, sender_jid);
  storage_db_bind_text(stmt, 3, stanza_xml);
  storage_db_bind_int64(stmt, 4, now_seconds());
  storage_db_bind_int64(stmt, 5, (long long)stanza_len);

  rc = storage_db_step(stmt);
  if (rc != SQLITE_DONE) {
    stump_er("offline_store: insert failed: %s", sqlite3_errmsg(db));
    return -1;
  }

  stump_d("offline_store: stored message from %s to %s (%zu bytes)", sender_jid,
          recipient_bare_jid, stanza_len);
  return 0;
}

int offline_count(const char* recipient_bare_jid) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;
  int count = 0;

  if (!recipient_bare_jid) {
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    return -1;
  }

  rc = storage_db_prepare(db, "SELECT COUNT(*) FROM offline_messages WHERE recipient_jid = ?",
                          &stmt);
  if (rc != 0) {
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, recipient_bare_jid);

  rc = storage_db_step(stmt);
  if (rc == SQLITE_ROW) {
    count = (int)storage_db_column_int64(stmt, 0);
  }

  return count;
}

long long offline_total_bytes(const char* recipient_bare_jid) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;
  long long total = 0;

  if (!recipient_bare_jid) {
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    return -1;
  }

  rc = storage_db_prepare(db,
                          "SELECT COALESCE(SUM(bytes_size), 0) FROM offline_messages WHERE "
                          "recipient_jid = ?",
                          &stmt);
  if (rc != 0) {
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, recipient_bare_jid);

  rc = storage_db_step(stmt);
  if (rc == SQLITE_ROW) {
    total = storage_db_column_int64(stmt, 0);
  }

  return total;
}

int offline_is_capped(const char* recipient_bare_jid) {
  int count = offline_count(recipient_bare_jid);
  if (count < 0) {
    return -1;
  }
  if (count >= OFFLINE_MAX_MESSAGES) {
    return 1;
  }

  long long total = offline_total_bytes(recipient_bare_jid);
  if (total < 0) {
    return -1;
  }
  if (total >= OFFLINE_MAX_BYTES) {
    return 1;
  }

  return 0;
}

int offline_list(const char* recipient_bare_jid, offline_msg_cb cb, void* ud) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (!recipient_bare_jid || !cb) {
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    return -1;
  }

  rc = storage_db_prepare(db,
                          "SELECT id, sender_jid, stanza_xml, received_at FROM offline_messages "
                          "WHERE recipient_jid = ? ORDER BY received_at ASC",
                          &stmt);
  if (rc != 0) {
    stump_er("offline_list: cannot prepare statement");
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, recipient_bare_jid);

  while ((rc = storage_db_step(stmt)) == SQLITE_ROW) {
    long long id         = storage_db_column_int64(stmt, 0);
    const char* sender_jid = storage_db_column_text_col(stmt, 1);
    const char* stanza_xml = storage_db_column_text_col(stmt, 2);
    long long received_at  = storage_db_column_int64(stmt, 3);

    if (sender_jid && stanza_xml) {
      cb(recipient_bare_jid, sender_jid, stanza_xml, received_at, ud);
    }
    (void)id;
  }

  if (rc != SQLITE_DONE) {
    stump_er("offline_list: step failed: %s", sqlite3_errmsg(db));
    return -1;
  }

  return 0;
}

int offline_delete(long long id) {
  sqlite3* db = NULL;
  storage_stmt_t* stmt = NULL;
  int rc;

  if (storage_db_open(&db) != 0) {
    return -1;
  }

  rc = storage_db_prepare(db, "DELETE FROM offline_messages WHERE id = ?", &stmt);
  if (rc != 0) {
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_int64(stmt, 1, id);

  rc = storage_db_step(stmt);
  if (rc != SQLITE_DONE) {
    stump_er("offline_delete: delete failed: %s", sqlite3_errmsg(db));
    return -1;
  }

  return 0;
}

int offline_delete_all(const char* recipient_bare_jid) {
  sqlite3* db = NULL;
  int rc;

  if (!recipient_bare_jid) {
    return -1;
  }

  if (storage_db_open(&db) != 0) {
    return -1;
  }

  storage_stmt_t* stmt = NULL;
  rc = storage_db_prepare(db, "DELETE FROM offline_messages WHERE recipient_jid = ?", &stmt);
  if (rc != 0) {
    return -1;
  }

  storage_db_reset(stmt);
  storage_db_bind_text(stmt, 1, recipient_bare_jid);

  rc = storage_db_step(stmt);
  if (rc != SQLITE_DONE) {
    stump_er("offline_delete_all: delete failed: %s", sqlite3_errmsg(db));
    return -1;
  }

  return storage_db_changes(db);
}

/* Callback context for offline_drain. */
typedef struct {
  int (*write_fn)(void* ud, const char* data, size_t len);
  void* write_ud;
} drain_ctx_t;

/* Callback to send a single offline message with <delay> stamp. */
static void drain_cb(const char* recipient_jid, const char* sender_jid, const char* stanza_xml,
                     long long received_at, void* ud) {
  (void)recipient_jid;

  drain_ctx_t* ctx = (drain_ctx_t*)ud;
  char datetime[64];

  if (format_datetime(datetime, sizeof(datetime), received_at) != 0) {
    stump_er("offline_drain: cannot format datetime for message from %s", sender_jid);
    return;
  }

  /* Build the delayed message:
   * Insert <delay> element after the opening <message> tag.
   * Original stanza: <message from='...' to='...' ...>...</message>
   * Delayed: <message from='...' to='...' ...><delay xmlns='urn:xmpp:delay' stamp='...'/>...</message>
   */
  char delayed[65536];
  size_t delayed_len = 0;

  /* Find the end of the opening tag and insert <delay> after it. */
  const char* gt = strchr(stanza_xml, '>');
  if (!gt) {
    /* Malformed stanza, send as-is */
    ctx->write_fn(ctx->write_ud, stanza_xml, strlen(stanza_xml));
    return;
  }

  size_t opening_len = (size_t)(gt - stanza_xml) + 1;
  size_t xml_len = strlen(stanza_xml);

  /* Copy opening tag */
  if (opening_len >= sizeof(delayed)) {
    ctx->write_fn(ctx->write_ud, stanza_xml, xml_len);
    return;
  }
  memcpy(delayed, stanza_xml, opening_len);
  delayed_len = opening_len;

  /* Add delay stamp */
  int rc = snprintf(delayed + delayed_len, sizeof(delayed) - delayed_len,
                   "<delay xmlns='urn:xmpp:delay' stamp='%s' from='%s'/>", datetime, recipient_jid);
  if (rc < 0 || (size_t)rc >= sizeof(delayed) - delayed_len) {
    ctx->write_fn(ctx->write_ud, stanza_xml, xml_len);
    return;
  }
  delayed_len += (size_t)rc;

  /* Copy rest of stanza (everything after the opening tag's closing '>') */
  size_t tail_len = xml_len - opening_len;
  if (delayed_len + tail_len >= sizeof(delayed)) {
    ctx->write_fn(ctx->write_ud, stanza_xml, xml_len);
    return;
  }
  memcpy(delayed + delayed_len, gt + 1, tail_len);
  delayed_len += tail_len;
  delayed[delayed_len] = '\0';

  ctx->write_fn(ctx->write_ud, delayed, delayed_len);
}

int offline_drain(const char* recipient_bare_jid,
                  int (*write_fn)(void* ud, const char* data, size_t len), void* write_ud) {
  drain_ctx_t ctx;
  ctx.write_fn = write_fn;
  ctx.write_ud = write_ud;

  int count = offline_count(recipient_bare_jid);
  if (count < 0) {
    return -1;
  }
  if (count == 0) {
    return 0;
  }

  stump_i("offline_drain: delivering %d offline messages to %s", count, recipient_bare_jid);

  if (offline_list(recipient_bare_jid, drain_cb, &ctx) != 0) {
    return -1;
  }

  if (offline_delete_all(recipient_bare_jid) < 0) {
    stump_er("offline_drain: failed to delete messages for %s", recipient_bare_jid);
    return -1;
  }

  stump_i("offline_drain: drained all offline messages for %s", recipient_bare_jid);
  return 0;
}