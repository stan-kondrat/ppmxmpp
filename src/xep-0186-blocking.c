/* XEP-0186: Blocking Command
 * https://xmpp.org/extensions/xep-0186.html
 *
 * Provides block/unblock functionality via IQ stanzas.
 * Blocked JIDs cannot send messages or presence to the blocking user.
 */
#include "xep-0186-blocking.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "strophe.h"
#include "storage/db_blocklist.h"
#include "xmpp_iq_buf.h"
#include "xmpp_iq_dispatch.h"
#include "xmpp_session.h"

/* Namespace for privacy lists / blocking. */
#define NS_PRIVACY "jabber:iq:privacy"

/* ------------------------------------------------------------------ */
/*  Error response helpers                                             */
/* ------------------------------------------------------------------ */

static void send_blocklist_error(xmpp_session_t* ctx, const char* iq_id,
                                  const char* err_type, const char* condition) {
  char buf[IQ_BUF_SIZE];
  size_t len = 0;
  int rc;
  if (iq_id) {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<iq type='error' id='%s'><error type='%s'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   iq_id, err_type, condition);
  } else {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<iq type='error'><error type='%s'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   err_type, condition);
  }
  if (rc == 0) iq_flush(ctx, buf, len);
}

static void send_blocklist_result(xmpp_session_t* ctx, const char* iq_id) {
  char buf[256];
  size_t len = 0;
  int rc = iq_append(buf, &len, sizeof(buf),
                     iq_id ? "<iq type='result' id='%s'/>" : "<iq type='result'/>",
                     iq_id ? iq_id : "");
  if (rc == 0) iq_flush(ctx, buf, len);
}

/* ------------------------------------------------------------------ */
/*  Block list XML serialisation helper                               */
/* ------------------------------------------------------------------ */

/* Context for serialising blocklist items into XML. */
typedef struct {
  char* buf;
  size_t len;
  size_t cap;
  int error;
} blocklist_xml_ctx_t;

static void append_block_item(blocklist_xml_ctx_t* bx, const char* blocked_jid) {
  if (iq_append(bx->buf, &bx->len, bx->cap, "<item type='jid' action='deny' value='%s'/>",
                blocked_jid) != 0) {
    bx->error = 1;
  }
}

static int blocklist_iter_cb(const char* blocked_jid, void* ud) {
  blocklist_xml_ctx_t* bx = (blocklist_xml_ctx_t*)ud;
  if (bx->error) return 1;
  append_block_item(bx, blocked_jid);
  return bx->error ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  IQ Handlers                                                       */
/* ------------------------------------------------------------------ */

/* Handle jabber:iq:privacy get — return the user's blocklist.
 * XEP-0186 §3.1: <iq type='get'><query xmlns='jabber:iq:privacy'/></iq>
 */
static iq_handler_result_t xep0186_handle_blocklist_get(xmpp_session_t* ctx,
                                                         xmpp_stanza_t* stanza,
                                                         xmpp_stanza_t* child,
                                                         const char* iq_id) {
  (void)stanza;
  (void)child;
  char owner[JID_BUF_SIZE];
  xmpp_session_bare_jid(ctx->bound_jid, owner, sizeof(owner));

  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("blocklist get: out of memory");
    return IQ_ERROR;
  }
  size_t len = 0;

  if (iq_append(buf, &len, IQ_BUF_SIZE,
                iq_id ? "<iq type='result' id='%s'><query xmlns='%s'>" : "<iq type='result'><query xmlns='%s'>",
                iq_id ? iq_id : "", NS_PRIVACY) != 0) {
    free(buf);
    return IQ_ERROR;
  }

  blocklist_xml_ctx_t bx = {buf, len, IQ_BUF_SIZE, 0};
  bx.len = len;

  if (storage_blocklist_list(owner, blocklist_iter_cb, &bx) != 0 || bx.error) {
    len = 0;
    iq_append(buf, &len, IQ_BUF_SIZE,
              iq_id ? "<iq type='error' id='%s'><error type='cancel'>"
                       "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                       "</error></iq>"
                     : "<iq type='error'><error type='cancel'>"
                       "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                       "</error></iq>",
              iq_id ? iq_id : "");
    iq_flush(ctx, buf, len);
    free(buf);
    return IQ_ERROR;
  }

  len = bx.len;
  if (iq_append(buf, &len, IQ_BUF_SIZE, "</query></iq>") != 0) {
    stump_er("blocklist get: response buffer overflow for '%s'", owner);
    free(buf);
    return IQ_ERROR;
  }

  stump_d("blocklist get owner=%s iq_id=%s", owner, iq_id ? iq_id : "(none)");
  iq_flush(ctx, buf, len);
  free(buf);
  return IQ_HANDLED;
}

/* Handle jabber:iq:privacy set <block> — block one or more JIDs.
 * XEP-0186 §3.2: <iq type='set'><block><item jid='...'/></block></iq>
 */
static iq_handler_result_t xep0186_handle_block(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                                 xmpp_stanza_t* child, const char* iq_id) {
  (void)stanza;

  char owner[JID_BUF_SIZE];
  xmpp_session_bare_jid(ctx->bound_jid, owner, sizeof(owner));

  /* Locate the <block> child element. */
  xmpp_stanza_t* block_el = xmpp_stanza_get_child_by_name(child, "block");
  if (!block_el) {
    /* Not a block request — let other handlers try. */
    return IQ_NOT_MINE;
  }

  /* Iterate all <item jid='...'> children. */
  xmpp_stanza_t* item = xmpp_stanza_get_children(block_el);
  int count = 0;
  while (item) {
    const char* name = xmpp_stanza_get_name(item);
    const char* jid = xmpp_stanza_get_attribute(item, "jid");
    if (name && strcmp(name, "item") == 0 && jid && jid[0] != '\0') {
      if (storage_blocklist_add(owner, jid) != 0) {
        send_blocklist_error(ctx, iq_id, "cancel", "internal-server-error");
        return IQ_ERROR;
      }
      stump_d("blocklist: %s blocked %s", owner, jid);
      count++;
    }
    item = xmpp_stanza_get_next(item);
  }

  if (count == 0) {
    send_blocklist_error(ctx, iq_id, "modify", "bad-request");
    return IQ_ERROR;
  }

  send_blocklist_result(ctx, iq_id);
  return IQ_HANDLED;
}

/* Handle jabber:iq:privacy set <unblock> — unblock one or more JIDs.
 * XEP-0186 §3.3: <iq type='set'><unblock><item jid='...'/></unblock></iq>
 */
static iq_handler_result_t xep0186_handle_unblock(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                                   xmpp_stanza_t* child, const char* iq_id) {
  (void)stanza;

  char owner[JID_BUF_SIZE];
  xmpp_session_bare_jid(ctx->bound_jid, owner, sizeof(owner));

  /* Locate the <unblock> child element. */
  xmpp_stanza_t* unblock_el = xmpp_stanza_get_child_by_name(child, "unblock");
  if (!unblock_el) {
    /* Not an unblock request — let other handlers try. */
    return IQ_NOT_MINE;
  }

  /* Iterate all <item jid='...'> children. */
  xmpp_stanza_t* item = xmpp_stanza_get_children(unblock_el);
  int count = 0;
  while (item) {
    const char* name = xmpp_stanza_get_name(item);
    const char* jid = xmpp_stanza_get_attribute(item, "jid");
    if (name && strcmp(name, "item") == 0 && jid && jid[0] != '\0') {
      if (storage_blocklist_remove(owner, jid) != 0) {
        send_blocklist_error(ctx, iq_id, "cancel", "internal-server-error");
        return IQ_ERROR;
      }
      stump_d("blocklist: %s unblocked %s", owner, jid);
      count++;
    }
    item = xmpp_stanza_get_next(item);
  }

  if (count == 0) {
    /* Unblock all: no item elements means unblock everyone. */
    /* For simplicity, require at least one item. */
    send_blocklist_error(ctx, iq_id, "modify", "bad-request");
    return IQ_ERROR;
  }

  send_blocklist_result(ctx, iq_id);
  return IQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/*  Handler registration                                              */
/* ------------------------------------------------------------------ */

static const iq_handler_entry_t blocking_handlers[] = {
    IQ_HANDLER(NS_PRIVACY, "get",  IQ_PRIORITY_NORMAL, xep0186_handle_blocklist_get),
    IQ_HANDLER(NS_PRIVACY, "set",  IQ_PRIORITY_NORMAL, xep0186_handle_block),
    IQ_HANDLER(NS_PRIVACY, "set",  IQ_PRIORITY_NORMAL, xep0186_handle_unblock),
    IQ_HANDLERS_END
};

int xep0186_init(void) {
  return iq_handler_register_all(blocking_handlers);
}