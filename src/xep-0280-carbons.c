/* XEP-0280: Message Carbons
 * https://xmpp.org/extensions/xep-0280.html
 *
 * §5    Client opts in/out per resource via <enable/>/<disable/> IQ set.
 * §7    Server copies outbound chat messages to all other carbons-enabled
 *       resources of the sender as <sent/> wrappers.
 * §8    Server copies inbound chat messages to all other carbons-enabled
 *       resources of the recipient as <received/> wrappers.
 */
#include "xep-0280-carbons.h"

#include <stdint.h>
#include <string.h>

#include "strophe.h"
#include "stumpless.h"
#include "xmpp_iq_buf.h"
#include "xmpp_iq_dispatch.h"
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

/* ------------------------------------------------------------------ */
/*  IQ Handler for carbons enable/disable                             */
/* ------------------------------------------------------------------ */

/* XEP-0280 §5: handle <iq type='set'><enable|disable xmlns='urn:xmpp:carbons:2'/></iq>. */
static iq_handler_result_t xep0280_handle_carbons_iq(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                              xmpp_stanza_t* child, const char* iq_id) {
  (void)child;

  xmpp_stanza_t* child_el = xmpp_stanza_get_children(stanza);
  while (child_el) {
    const char* cname = xmpp_stanza_get_name(child_el);
    if (cname && (strcmp(cname, "enable") == 0 || strcmp(cname, "disable") == 0)) {
      int enable = (strcmp(cname, "enable") == 0);
      ctx->carbons_enabled = enable;
      xmpp_session_table_update_carbons(ctx->bound_jid, enable);
      stump_d("carbons iq: %s for %s", enable ? "enable" : "disable", ctx->bound_jid);
      break;
    }
    child_el = xmpp_stanza_get_next(child_el);
  }

  if (iq_id) {
    char buf[128];
    size_t len = 0;
    iq_append(buf, &len, sizeof(buf), "<iq type='result' id='%s'/>", iq_id);
    iq_flush(ctx, buf, len);
  }
  return IQ_HANDLED;
}

/* Handler registration table. */
static const iq_handler_entry_t carbons_handlers[] = {
    IQ_HANDLER("urn:xmpp:carbons:2", "set", IQ_PRIORITY_NORMAL, xep0280_handle_carbons_iq),
    IQ_HANDLERS_END
};

/* Module initialization — registers handlers with the dispatcher. */
int xep0280_init(void) {
    return iq_handler_register_all(carbons_handlers);
}
