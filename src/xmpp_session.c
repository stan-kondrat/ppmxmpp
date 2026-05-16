#include "xmpp_session.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "stumpless.h"
#include "storage/db_offline.h"

/* ------------------------------------------------------------------ */
/*  Internal types                                                     */
/* ------------------------------------------------------------------ */

#define SESSION_TABLE_CAP 256

typedef struct {
  char bound_jid[3073];
  xmpp_write_fn write_fn;
  void* write_ud;
  int priority;         /* <presence><priority> value, default 0 */
  uint64_t last_active; /* monotonic nanoseconds; updated on register and activity */
  int carbons_enabled;  /* XEP-0280 carbons opt-in for this resource */
} session_entry_t;

session_entry_t g_sessions[SESSION_TABLE_CAP];
int g_session_count = 0;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Extract bare JID (user@domain) from a full JID. */
void xmpp_session_bare_jid(const char* full, char* out, size_t out_size) {
  const char* slash = strchr(full, '/');
  size_t len = slash ? (size_t)(slash - full) : strlen(full);
  if (len >= out_size) len = out_size - 1;
  memcpy(out, full, len);
  out[len] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void xmpp_session_table_register(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* write_ud) {
  if (!ctx->bound_jid[0]) return;

  uint64_t now = monotonic_ns();

  /* Replace existing entry for this JID (re-bind). */
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, ctx->bound_jid) == 0) {
      g_sessions[i].write_fn = write_fn;
      g_sessions[i].write_ud = write_ud;
      g_sessions[i].last_active = now;
      g_sessions[i].carbons_enabled = ctx->carbons_enabled;
      return;
    }
  }

  if (g_session_count >= SESSION_TABLE_CAP) {
    stump_er("session: table full, cannot register %s", ctx->bound_jid);
    return;
  }

  session_entry_t* e = &g_sessions[g_session_count++];
  memset(e, 0, sizeof(*e));
  strncpy(e->bound_jid, ctx->bound_jid, sizeof(e->bound_jid) - 1);
  e->write_fn = write_fn;
  e->write_ud = write_ud;
  e->last_active = now;
  e->carbons_enabled = ctx->carbons_enabled;
  stump_d("session: registered %s (%d sessions)", e->bound_jid, g_session_count);

  /* Drain offline messages for this user (XEP-0160). */
  char bare_jid[3073];
  xmpp_session_bare_jid(ctx->bound_jid, bare_jid, sizeof(bare_jid));
  stump_i("session: about to drain offline messages for %s", bare_jid);
  if (offline_drain(bare_jid, write_fn, write_ud) != 0) {
    stump_er("session: failed to drain offline messages for %s", bare_jid);
  }
}

void xmpp_session_table_unregister(const char* bound_jid) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      /* Swap with last entry to keep table compact. */
      g_sessions[i] = g_sessions[--g_session_count];
      stump_d("session: unregistered %s (%d sessions)", bound_jid, g_session_count);
      return;
    }
  }
}

int xmpp_session_table_write(const char* bound_jid, const char* data, size_t len) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      return g_sessions[i].write_fn(g_sessions[i].write_ud, data, len);
    }
  }
  stump_er("session: write failed, session not found for '%s'", bound_jid);
  return -1;
}

void xmpp_session_table_update_priority(const char* bound_jid, int priority) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      g_sessions[i].priority = priority;
      return;
    }
  }
}

void xmpp_session_table_touch(const char* bound_jid) {
  uint64_t now = monotonic_ns();
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      g_sessions[i].last_active = now;
      return;
    }
  }
}

int xmpp_session_table_best_resource(const char* bare_jid, char* out_full_jid, size_t out_size) {
  int best = -1;
  for (int i = 0; i < g_session_count; i++) {
    char bare[3073];
    xmpp_session_bare_jid(g_sessions[i].bound_jid, bare, sizeof(bare));
    if (strcmp(bare, bare_jid) != 0) continue;
    if (best < 0) {
      best = i;
      continue;
    }
    if (g_sessions[i].priority > g_sessions[best].priority) {
      best = i;
    } else if (g_sessions[i].priority == g_sessions[best].priority &&
               g_sessions[i].last_active > g_sessions[best].last_active) {
      best = i;
    }
  }
  if (best < 0) return -1;
  strncpy(out_full_jid, g_sessions[best].bound_jid, out_size - 1);
  out_full_jid[out_size - 1] = '\0';
  return 0;
}

void xmpp_session_table_broadcast_except(const char* bare_jid, const char* exclude_full_jid,
                                         const char* data, size_t len) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, exclude_full_jid) == 0) continue;
    char bare[3073];
    xmpp_session_bare_jid(g_sessions[i].bound_jid, bare, sizeof(bare));
    if (strcmp(bare, bare_jid) == 0) {
      g_sessions[i].write_fn(g_sessions[i].write_ud, data, len);
    }
  }
}

int xmpp_session_table_is_registered(const char* bound_jid) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) return 1;
  }
  return 0;
}

void xmpp_session_table_reset_all(void) {
  g_session_count = 0;
}

/* ------------------------------------------------------------------ */
/*  Package-internal access                                            */
/*                                                                     */
/*  xmpp_presence.c needs to iterate g_sessions directly for          */
/*  broadcast_to_bare_jid and broadcast_presence. Rather than         */
/*  exposing the array, we provide two targeted iterators.            */
/* ------------------------------------------------------------------ */

/* Deliver stanza to all online resources of target_bare. */
void xmpp_session_table_broadcast_to_bare(const char* target_bare, const char* data, size_t len) {
  for (int i = 0; i < g_session_count; i++) {
    char bare[3073];
    xmpp_session_bare_jid(g_sessions[i].bound_jid, bare, sizeof(bare));
    if (strcmp(bare, target_bare) == 0) {
      g_sessions[i].write_fn(g_sessions[i].write_ud, data, len);
    }
  }
}

/* Call cb(full_jid, write_fn, write_ud, ud) for every session whose bare JID
 * matches bare_jid and whose full JID differs from exclude_full_jid.
 * Used by broadcast_presence to deliver to own other resources. */
void xmpp_session_table_for_each_resource(const char* bare_jid, const char* exclude_full_jid,
                                           void (*cb)(const char* full_jid, xmpp_write_fn write_fn,
                                                      void* write_ud, const void* ud),
                                           const void* ud) {
  for (int i = 0; i < g_session_count; i++) {
    if (exclude_full_jid && strcmp(g_sessions[i].bound_jid, exclude_full_jid) == 0) continue;
    char bare[3073];
    xmpp_session_bare_jid(g_sessions[i].bound_jid, bare, sizeof(bare));
    if (strcmp(bare, bare_jid) == 0) {
      cb(g_sessions[i].bound_jid, g_sessions[i].write_fn, g_sessions[i].write_ud, ud);
    }
  }
}
