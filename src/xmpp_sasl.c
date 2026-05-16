#include "xmpp_sasl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/*  Base64 decode (libstrophe's b64 helper is not exported)           */
/* ------------------------------------------------------------------ */

static int b64_decode(const char* in, size_t in_len, unsigned char** out, size_t* out_len) {
  static const char b64_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  *out = malloc(in_len);
  if (!*out) {
    stump_er("b64_decode: out of memory");
    return -1;
  }

  int state = 0;
  int val = 0;
  int decoded_len = 0;

  for (size_t i = 0; i < in_len; i++) {
    const char* c = strchr(b64_table, in[i]);
    if (!c) {
      continue;
    }
    val = (val << 6) | (int)(c - b64_table);
    state++;
    if (state == 4) {
      (*out)[decoded_len++] = (unsigned char)((val >> 16) & 0xFF);
      (*out)[decoded_len++] = (unsigned char)((val >> 8) & 0xFF);
      (*out)[decoded_len++] = (unsigned char)(val & 0xFF);
      state = 0;
      val = 0;
    }
  }
  if (state > 0) {
    val <<= (4 - state) * 6;
    if (state >= 2) {
      (*out)[decoded_len++] = (unsigned char)((val >> 16) & 0xFF);
    }
    if (state == 3) {
      (*out)[decoded_len++] = (unsigned char)((val >> 8) & 0xFF);
    }
  }

  *out_len = (size_t)decoded_len;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Constant-time comparison                                          */
/* ------------------------------------------------------------------ */

/* Returns 1 if the first len bytes of a and b are equal.
 * Runs in constant time regardless of content to resist timing attacks. */
static int ct_memeq(const char* a, const char* b, size_t len) {
  unsigned char diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
  }
  return diff == 0;
}

/* ------------------------------------------------------------------ */
/*  JID localpart validation                                           */
/* ------------------------------------------------------------------ */

/* Returns 1 if the localpart contains a character forbidden by RFC 7622 §3.3.1.
 */
static int jid_localpart_has_forbidden(const char* s) {
  static const char forbidden[] = "\"&'/:;<>@";
  for (; *s; s++) {
    if (strchr(forbidden, *s)) {
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  SASL PLAIN authentication                                          */
/* ------------------------------------------------------------------ */

sasl_rc_t handle_sasl_plain(xmpp_session_t* ctx, const char* b64_text, xmpp_write_fn write_fn, void* ud) {
  (void)write_fn;
  (void)ud;

  unsigned char* decoded = NULL;
  size_t len = 0;
  if (b64_decode(b64_text, strlen(b64_text), &decoded, &len) != 0) {
    stump_er("SASL PLAIN: base64 decode failed");
    return -2;
  }
  const char* data = (const char*)decoded;

  /* RFC 4616 §2 message format: [authzid] NUL authcid NUL passwd */
  const char* authzid = data;
  const char* authcid = NULL;
  const char* password = NULL;
  const char* p = data;

  /* Find first NUL — end of authzid field. */
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '\0') {
      p = data + i + 1;
      break;
    }
  }
  if (p == data) {
    stump_er("SASL PLAIN: malformed credentials (no NUL)");
    free(decoded);
    return -2;
  }
  size_t authzid_len = (size_t)(p - data) - 1; /* bytes before first NUL */

  /* Find second NUL — end of authcid field. */
  for (size_t i = (size_t)(p - data); i < len; i++) {
    if (data[i] == '\0') {
      authcid = p;
      p = data + i + 1;
      break;
    }
  }
  if (!authcid || p == data + len) {
    stump_er("SASL PLAIN: malformed credentials");
    free(decoded);
    return -2;
  }
  password = p;

  /* RFC 4616 §2: authcid MUST be non-empty (1*SAFE). */
  if (strlen(authcid) == 0) {
    stump_er("SASL PLAIN: empty authcid");
    free(decoded);
    return -2;
  }

  /* RFC 7622 §3.3.1: reject forbidden localpart characters. */
  if (jid_localpart_has_forbidden(authcid)) {
    stump_d("SASL PLAIN: forbidden character in authcid");
    free(decoded);
    return -2;
  }

  int ac_len = (int)(strlen(authcid) < sizeof(ctx->authcid) - 1 ? strlen(authcid)
                                                                : sizeof(ctx->authcid) - 1);
  memcpy(ctx->authcid, authcid, (size_t)ac_len);
  ctx->authcid[ac_len] = '\0';

  /* RFC 7622 §3.1: localpart + '@' + domainpart ≤ 2047 octets. */
  char bare_jid[2048];
  int n = snprintf(bare_jid, sizeof(bare_jid), "%s@%s", authcid, ctx->domain);
  if (n < 0 || (size_t)n >= sizeof(bare_jid)) {
    stump_er("SASL PLAIN: JID too long");
    free(decoded);
    return -2;
  }

  sqlite3* db;
  if (storage_db_open(&db) != 0) {
    stump_er("SASL PLAIN: cannot open database");
    free(decoded);
    return SASL_TERMINAL;
  }

  storage_stmt_t* stmt = NULL;
  const char* sql = "SELECT password_plain, disabled FROM users WHERE jid = ?";
  if (storage_db_prepare(db, sql, &stmt) != 0) {
    stump_er("SASL PLAIN: prepare failed");
    free(decoded);
    storage_db_close();
    return SASL_TERMINAL;
  }

  storage_db_bind_text(stmt, 1, bare_jid);
  int rc = storage_db_step(stmt);
  if (rc != SQLITE_ROW) {
    stump_i("SASL PLAIN: user not found: %s", bare_jid);
    storage_db_reset(stmt);
    free(decoded);
    storage_db_close();
    return -1;
  }

  char* stored_pw = storage_db_column_text_copy(stmt, 0);
  int disabled = (int)storage_db_column_int64(stmt, 1);
  storage_db_reset(stmt);

  if (disabled) {
    stump_i("SASL PLAIN: account disabled: %s", bare_jid);
    free(stored_pw);
    free(decoded);
    storage_db_close();
    return -2;
  }

  /* password is not NUL-terminated — it occupies the tail of the decoded
   * buffer. */
  size_t pw_len = (size_t)(data + len - password);
  if (!stored_pw || strlen(stored_pw) != pw_len || !ct_memeq(stored_pw, password, pw_len)) {
    stump_i("SASL PLAIN: bad password for: %s", bare_jid);
    free(stored_pw);
    free(decoded);
    storage_db_close();
    return -1;
  }

  free(stored_pw);

  /* RFC 4616 §2: if authzid is non-empty it must equal the derived identity
   * (bare_jid). */
  if (authzid_len > 0) {
    char authzid_str[2048];
    size_t az_copy = authzid_len < sizeof(authzid_str) - 1 ? authzid_len : sizeof(authzid_str) - 1;
    memcpy(authzid_str, authzid, az_copy);
    authzid_str[az_copy] = '\0';
    if (strcmp(authzid_str, bare_jid) != 0) {
      stump_i("SASL PLAIN: authzid mismatch: '%s' vs '%s'", authzid_str, bare_jid);
      free(decoded);
      storage_db_close();
      return -2;
    }
  }

  stump_i("SASL PLAIN: authenticated: %s", bare_jid);
  free(decoded);
  storage_db_close();
  return 0;
}
