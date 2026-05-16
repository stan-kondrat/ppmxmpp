#include "xep-0280-carbons.h"

#include <stdint.h>
#include <string.h>

#include "stumpless.h"
#include "xmpp_session.h"

/* ------------------------------------------------------------------ */
/*  Access to xmpp_session.c internals                                */
/* ------------------------------------------------------------------ */

#define SESSION_TABLE_CAP 256

typedef struct {
  char bound_jid[3073];
  xmpp_write_fn write_fn;
  void* write_ud;
  int priority;
  uint64_t last_active;
  int carbons_enabled;
} session_entry_t;

extern session_entry_t g_sessions[SESSION_TABLE_CAP];

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void xmpp_session_table_update_carbons(const char* bound_jid, int enabled) {
  for (int i = 0; i < g_session_count; i++) {
    if (strcmp(g_sessions[i].bound_jid, bound_jid) == 0) {
      g_sessions[i].carbons_enabled = enabled ? 1 : 0;
      stump_d("carbons: %s carbons for %s", enabled ? "enabled" : "disabled", bound_jid);
      return;
    }
  }
}

void xmpp_session_table_for_each_carbon_resource(const char* bare_jid,
                                                  const char* exclude_full_jid,
                                                  void (*cb)(const char* full_jid,
                                                             xmpp_write_fn write_fn,
                                                             void* write_ud,
                                                             const void* ud),
                                                  const void* ud) {
  for (int i = 0; i < g_session_count; i++) {
    if (exclude_full_jid && strcmp(g_sessions[i].bound_jid, exclude_full_jid) == 0) continue;
    char bare[3073];
    const char* slash = strchr(g_sessions[i].bound_jid, '/');
    size_t len = slash ? (size_t)(slash - g_sessions[i].bound_jid) :
                         strlen(g_sessions[i].bound_jid);
    if (len >= sizeof(bare)) len = sizeof(bare) - 1;
    memcpy(bare, g_sessions[i].bound_jid, len);
    bare[len] = '\0';
    if (strcmp(bare, bare_jid) != 0) continue;
    if (!g_sessions[i].carbons_enabled) continue;
    cb(g_sessions[i].bound_jid, g_sessions[i].write_fn, g_sessions[i].write_ud, ud);
  }
}