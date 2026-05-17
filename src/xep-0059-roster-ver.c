#include "xep-0059-roster-ver.h"
#include "storage/db.h"
#include "storage/db_roster.h"
#include "log.h"
#include "xmpp_iq_buf.h"

#include <mbedtls/md.h>
#include <stdlib.h>
#include <string.h>

#define ROSTER_VER_BUF_INITIAL 4096

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void roster_ver_append_item(roster_ver_ctx_t* ctx, const char* contact_jid,
                            const char* name, const char* subscription,
                            const char** groups, int gc) {
  if (!ctx || !contact_jid || !subscription) return;
  if (ctx->error) return;

  /* Canonical order: contact_jid (sort key), subscription, name, groups. */
  if (ctx->len > 0) {
    if (iq_append(ctx->buf, &ctx->len, ctx->cap, "|") != 0) {
      ctx->error = 1;
      return;
    }
  }
  if (iq_append(ctx->buf, &ctx->len, ctx->cap, "%s", contact_jid) != 0) {
    ctx->error = 1;
    return;
  }
  if (iq_append(ctx->buf, &ctx->len, ctx->cap, "|%s", subscription) != 0) {
    ctx->error = 1;
    return;
  }
  if (name && name[0]) {
    if (iq_append(ctx->buf, &ctx->len, ctx->cap, "|%s", name) != 0) {
      ctx->error = 1;
      return;
    }
  }
  for (int i = 0; i < gc; i++) {
    if (groups[i] && groups[i][0]) {
      if (iq_append(ctx->buf, &ctx->len, ctx->cap, ",%s", groups[i]) != 0) {
        ctx->error = 1;
        return;
      }
    }
  }
  (void)name;
}

int roster_ver_finalise(roster_ver_ctx_t* ctx) {
  if (!ctx) return -1;
  if (ctx->len >= ctx->cap) {
    ctx->error = 1;
    return -1;
  }
  ctx->buf[ctx->len] = '\0';
  return ctx->error ? -1 : 0;
}

int roster_ver_compute_sha256(const char* data, size_t data_len, char* out_ver) {
  if (!data || !out_ver) return -1;

  unsigned char hash[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);

  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) {
    mbedtls_md_free(&ctx);
    return -1;
  }

  if (mbedtls_md_setup(&ctx, info, 0) != 0) {
    mbedtls_md_free(&ctx);
    return -1;
  }
  if (mbedtls_md_update(&ctx, (const unsigned char*)data, data_len) != 0) {
    mbedtls_md_free(&ctx);
    return -1;
  }
  if (mbedtls_md_finish(&ctx, hash) != 0) {
    mbedtls_md_free(&ctx);
    return -1;
  }
  mbedtls_md_free(&ctx);

  /* Hex-encode. */
  for (int i = 0; i < 32; i++) {
    int n = snprintf(out_ver + (ptrdiff_t)(i * 2), 3, "%02x", hash[i]);
    if (n < 2) {
      out_ver[0] = '\0';
      return -1;
    }
  }
  out_ver[64] = '\0';
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Storage helpers (private)                                         */
/* ------------------------------------------------------------------ */

/* Rebuild canonical string from all current roster items and store SHA-256. */
static int rebuild_and_store_ver(sqlite3* db, const char* owner_jid) {
  /* Collect all items into a ver_ctx. */
  char* canonical = malloc(ROSTER_VER_BUF_INITIAL);
  if (!canonical) return -1;

  roster_ver_ctx_t vctx = {canonical, 0, ROSTER_VER_BUF_INITIAL, 0};

  storage_stmt_t* stmt = NULL;
  int rc = storage_db_prepare(db,
                              "SELECT contact_jid, name, subscription"
                              " FROM roster"
                              " WHERE owner_jid = ?"
                              " ORDER BY contact_jid ASC",
                              &stmt);
  if (rc != 0) {
    free(canonical);
    return -1;
  }
  storage_db_bind_text(stmt, 1, owner_jid);

  while ((rc = storage_db_step(stmt)) == SQLITE_ROW) {
    char* cjid = storage_db_column_text_copy(stmt, 0);
    char* name = storage_db_column_text_copy(stmt, 1);
    char* sub  = storage_db_column_text_copy(stmt, 2);

    const char* groups[32];
    int gc = storage_roster_get_groups(owner_jid, cjid, groups, 32);

    roster_ver_append_item(&vctx, cjid ? cjid : "", name ? name : "", sub ? sub : "none",
                           groups, gc);
    free(cjid);
    free(name);
    free(sub);

    if (vctx.error) break;
  }
  storage_db_reset(stmt);

  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    free(canonical);
    return -1;
  }

  if (roster_ver_finalise(&vctx) != 0) {
    /* Buffer too small — unlikely but handle gracefully. */
    stump_er("roster_ver: canonical buffer overflow for '%s'", owner_jid);
    free(canonical);
    return -1;
  }

  char ver_hex[ROSTER_VER_SIZE];
  if (roster_ver_compute_sha256(canonical, vctx.len, ver_hex) != 0) {
    free(canonical);
    return -1;
  }
  free(canonical);

  /* Upsert into roster_ver. */
  storage_stmt_t* up = NULL;
  rc = storage_db_prepare(db,
                          "INSERT OR REPLACE INTO roster_ver (owner_jid, ver)"
                          " VALUES (?, ?)",
                          &up);
  if (rc != 0) return -1;
  storage_db_bind_text(up, 1, owner_jid);
  storage_db_bind_text(up, 2, ver_hex);
  rc = storage_db_step(up);
  storage_db_reset(up);
  if (rc != SQLITE_DONE) return -1;

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Public storage API                                                */
/* ------------------------------------------------------------------ */

int roster_ver_increment(const char* owner_jid) {
  if (!owner_jid) return -1;

  sqlite3* db;
  if (storage_db_open(&db) != 0) {
    stump_er("roster_ver_increment: cannot open database");
    return -1;
  }

  int rc = rebuild_and_store_ver(db, owner_jid);
  storage_db_close();
  return rc;
}

int roster_ver_get(const char* owner_jid, char* out_ver) {
  if (!owner_jid || !out_ver) return -1;

  sqlite3* db;
  if (storage_db_open(&db) != 0) {
    stump_er("roster_ver_get: cannot open database");
    return -1;
  }

  storage_stmt_t* stmt = NULL;
  int rc = storage_db_prepare(db,
                              "SELECT ver FROM roster_ver WHERE owner_jid = ?",
                              &stmt);
  if (rc != 0) {
    storage_db_close();
    return -1;
  }
  storage_db_bind_text(stmt, 1, owner_jid);
  rc = storage_db_step(stmt);
  if (rc == SQLITE_ROW) {
    const char* v = storage_db_column_text_col(stmt, 0);
    if (!v) {
      storage_db_reset(stmt);
      storage_db_close();
      stump_er("roster_ver_get: NULL ver for owner '%s' — DB inconsistency", owner_jid);
      return -1;
    }
    /* Copy before closing — storage_db_column_text_col returns a pointer into
     * SQLite's buffer which is freed when the connection closes. */
    strncpy(out_ver, v, ROSTER_VER_SIZE - 1);
    out_ver[ROSTER_VER_SIZE - 1] = '\0';
    storage_db_reset(stmt);
    storage_db_close();
    return 0;
  }
  storage_db_reset(stmt);
  storage_db_close();
  return (rc == SQLITE_DONE) ? 1 : -1;
}

int roster_ver_peek(const char* owner_jid, char* out_ver) {
  return roster_ver_get(owner_jid, out_ver);
}