#include "xmpp_presence.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "storage/roster.h"
#include "strophe.h"
#include "stumpless.h"
#include "xmpp_iq_buf.h"

/* ------------------------------------------------------------------ */
/*  Session registry                                                   */
/* ------------------------------------------------------------------ */

#define SESSION_TABLE_CAP 256

typedef struct {
  char bound_jid[3073];
  xmpp_write_fn write_fn;
  void* write_ud;
} session_entry_t;

static session_entry_t g_sessions[SESSION_TABLE_CAP];
static int g_session_count = 0;

void presence_session_register(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* write_ud) {
  if (!ctx->bound_jid[0]) return;

  /* Replace existing entry for this JID (re-bind). */
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, ctx->bound_jid) == 0) {
      g_sessions[i].write_fn = write_fn;
      g_sessions[i].write_ud = write_ud;
      return;
    }
  }

  if (g_session_count >= SESSION_TABLE_CAP) {
    stump_er("presence: session table full, cannot register %s", ctx->bound_jid);
    return;
  }

  session_entry_t* e = &g_sessions[g_session_count++];
  strncpy(e->bound_jid, ctx->bound_jid, sizeof(e->bound_jid) - 1);
  e->bound_jid[sizeof(e->bound_jid) - 1] = '\0';
  e->write_fn = write_fn;
  e->write_ud = write_ud;
  stump_d("presence: registered %s (%d sessions)", e->bound_jid, g_session_count);
}

void presence_session_reset_all(void) {
  g_session_count = 0;
}

void presence_session_unregister(const char* bound_jid) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      /* Swap with last entry to keep table compact. */
      g_sessions[i] = g_sessions[--g_session_count];
      stump_d("presence: unregistered %s (%d sessions)", bound_jid, g_session_count);
      return;
    }
  }
}

int presence_session_write(const char* bound_jid, const char* data, size_t len) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      return g_sessions[i].write_fn(g_sessions[i].write_ud, data, len);
    }
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Extract bare JID (user@domain) from a full JID. */
static void bare_jid_from_full(const char* full, char* out, size_t out_size) {
  const char* slash = strchr(full, '/');
  size_t len = slash ? (size_t)(slash - full) : strlen(full);
  if (len >= out_size) len = out_size - 1;
  memcpy(out, full, len);
  out[len] = '\0';
}

/* Broadcast a presence stanza string to all registered sessions whose
 * bare JID matches target_bare (i.e. all resources of that user). */
static void broadcast_to_bare_jid(const char* target_bare, const char* stanza,
                                   size_t stanza_len) {
  for (int i = 0; i < g_session_count; i++) {
    char bare[3073];
    bare_jid_from_full(g_sessions[i].bound_jid, bare, sizeof(bare));
    if (strcmp(bare, target_bare) == 0) {
      g_sessions[i].write_fn(g_sessions[i].write_ud, stanza, stanza_len);
    }
  }
}

/* Callback context for roster_list broadcast. */
typedef struct {
  const char* stanza;
  size_t stanza_len;
} broadcast_ctx_t;

/* storage_roster_item_cb: deliver presence to all online resources of
 * each contact whose subscription is "from" or "both". */
static int broadcast_to_subscriber(const storage_roster_item_t* item, const char** groups,
                                    int group_count, void* ud) {
  (void)groups;
  (void)group_count;
  broadcast_ctx_t* bc = (broadcast_ctx_t*)ud;

  /* RFC 6121 §4.2.2: only broadcast to contacts subscribed to our presence
   * (subscription "from" or "both" on our roster means THEY subscribed to us). */
  if (strcmp(item->subscription, "from") != 0 && strcmp(item->subscription, "both") != 0) {
    return 0;
  }

  broadcast_to_bare_jid(item->contact_jid, bc->stanza, bc->stanza_len);
  return 0;
}

/* Build a <presence> or <presence type='unavailable'> stanza from a full JID.
 * Returns allocated buffer (caller must free) and sets *out_len. */
static char* build_presence_stanza(const char* from_full_jid, const char* type, size_t* out_len) {
  char* buf = malloc(IQ_BUF_SIZE);
  if (!buf) return NULL;
  size_t len = 0;
  int rc;
  if (type) {
    rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s' type='%s'/>",
                   from_full_jid, type);
  } else {
    rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s'/>", from_full_jid);
  }
  if (rc != 0) {
    free(buf);
    return NULL;
  }
  *out_len = len;
  return buf;
}

/* Broadcast presence (initial or unavailable) to:
 *   1. All contacts with subscription "from" or "both" (their online resources).
 *   2. All other online resources of the same user.
 * from_bare_jid: the sender's bare JID (used to look up the roster).
 * from_full_jid: the sender's full JID (used as the from= attribute).
 * type: NULL for available, "unavailable" for unavailable. */
static void broadcast_presence(const char* from_bare_jid, const char* from_full_jid,
                                 const char* type) {
  size_t stanza_len;
  char* stanza = build_presence_stanza(from_full_jid, type, &stanza_len);
  if (!stanza) {
    stump_er("presence: out of memory building stanza for %s", from_full_jid);
    return;
  }

  /* 1. Roster contacts. */
  sqlite3* db;
  if (storage_db_open(&db) == 0) {
    broadcast_ctx_t bc = {stanza, stanza_len};
    storage_roster_list(from_bare_jid, broadcast_to_subscriber, &bc);
    storage_db_close();
  } else {
    stump_er("presence: cannot open DB for broadcast from %s", from_bare_jid);
  }

  /* 2. Own other resources (RFC 6121 §4.2.2 / §4.4.2). */
  for (int i = 0; i < g_session_count; i++) {
    char bare[3073];
    bare_jid_from_full(g_sessions[i].bound_jid, bare, sizeof(bare));
    if (strcmp(bare, from_bare_jid) == 0 &&
        strcmp(g_sessions[i].bound_jid, from_full_jid) != 0) {
      g_sessions[i].write_fn(g_sessions[i].write_ud, stanza, stanza_len);
    }
  }

  free(stanza);
}

/* ------------------------------------------------------------------ */
/*  Presence stanza handler                                            */
/* ------------------------------------------------------------------ */

void xmpp_presence_handle(xmpp_session_t* ctx, xmpp_stanza_t* stanza) {
  const char* type = xmpp_stanza_get_attribute(stanza, "type");
  const char* to   = xmpp_stanza_get_attribute(stanza, "to");

  char bare_jid[3073];
  bare_jid_from_full(ctx->bound_jid, bare_jid, sizeof(bare_jid));

  /* Directed presence: to= attribute is set. RFC 6121 §4.6. */
  if (to && to[0] != '\0') {
    size_t len = 0;
    char* buf = malloc(IQ_BUF_SIZE);
    if (!buf) return;
    int rc;
    if (type) {
      rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s' to='%s' type='%s'/>",
                     ctx->bound_jid, to, type);
    } else {
      rc = iq_append(buf, &len, IQ_BUF_SIZE, "<presence from='%s' to='%s'/>",
                     ctx->bound_jid, to);
    }
    if (rc == 0) {
      /* Try full JID first; fall back to broadcasting to all resources of bare JID. */
      if (presence_session_write(to, buf, len) != 0) {
        broadcast_to_bare_jid(to, buf, len);
      }
    }
    free(buf);
    return;
  }

  /* Unavailable presence: RFC 6121 §4.4. */
  if (type && strcmp(type, "unavailable") == 0) {
    broadcast_presence(bare_jid, ctx->bound_jid, "unavailable");
    presence_session_unregister(ctx->bound_jid);
    return;
  }

  /* Initial presence (no type, no to): RFC 6121 §4.2.
   * Register the session now — a client is not considered available until
   * it sends initial presence, so we defer registration to this point. */
  if (!type || type[0] == '\0') {
    presence_session_register(ctx, ctx->write_fn, ctx->write_ud);
    broadcast_presence(bare_jid, ctx->bound_jid, NULL);
    return;
  }

  /* Other types (subscribe, subscribed, etc.) are Step 9 — ignore for now. */
  stump_d("presence: unhandled type='%s' from %s (Step 9)", type, ctx->bound_jid);
}

void xmpp_presence_on_disconnect(const char* bound_jid, const char* bare_jid) {
  /* Only broadcast if this session was registered (i.e. had sent initial presence). */
  int registered = 0;
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      registered = 1;
      break;
    }
  }
  if (!registered) return;

  /* RFC 6121 §4.4.2: server generates unavailable on ungraceful disconnect. */
  broadcast_presence(bare_jid, bound_jid, "unavailable");
  presence_session_unregister(bound_jid);
}
