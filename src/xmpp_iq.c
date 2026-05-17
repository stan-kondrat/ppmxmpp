#include "xmpp_iq.h"

#include <stdlib.h>
#include <string.h>

#include "storage/db_roster.h"
#include "strophe.h"
#include "log.h"
#include "xmpp_iq_buf.h"
#include "xep-0059-roster-ver.h"

/* ------------------------------------------------------------------ */
/*  Error response helper                                             */
/* ------------------------------------------------------------------ */

static void send_iq_error(xmpp_session_t* ctx, const char* iq_id,
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

static void send_iq_result(xmpp_session_t* ctx, const char* iq_id) {
  char buf[256];
  size_t len = 0;
  int rc;
  if (iq_id) {
    rc = iq_append(buf, &len, sizeof(buf), "<iq type='result' id='%s'/>", iq_id);
  } else {
    rc = iq_append(buf, &len, sizeof(buf), "<iq type='result'/>");
  }
  if (rc == 0) iq_flush(ctx, buf, len);
}

/* ------------------------------------------------------------------ */
/*  Roster XML serialisation helpers                                  */
/* ------------------------------------------------------------------ */

typedef struct {
  char* buf;
  size_t len;
  size_t cap;
  int error;
} roster_xml_ctx_t;

static void append_item_xml(roster_xml_ctx_t* rx, const storage_roster_item_t* item,
                            const char** groups, int gc) {
  /* <item jid='...' [name='...'] subscription='...' [ask='subscribe']/> */
  if (item->name[0]) {
    if (iq_append(rx->buf, &rx->len, rx->cap, "<item jid='%s' name='%s' subscription='%s'",
                  item->contact_jid, item->name, item->subscription) != 0) {
      rx->error = 1;
      return;
    }
  } else {
    if (iq_append(rx->buf, &rx->len, rx->cap, "<item jid='%s' subscription='%s'", item->contact_jid,
                  item->subscription) != 0) {
      rx->error = 1;
      return;
    }
  }
  if (item->ask) {
    if (iq_append(rx->buf, &rx->len, rx->cap, " ask='subscribe'") != 0) {
      rx->error = 1;
      return;
    }
  }
  if (gc == 0) {
    if (iq_append(rx->buf, &rx->len, rx->cap, "/>") != 0) {
      rx->error = 1;
    }
    return;
  }
  if (iq_append(rx->buf, &rx->len, rx->cap, ">") != 0) {
    rx->error = 1;
    return;
  }
  for (int i = 0; i < gc; i++) {
    if (iq_append(rx->buf, &rx->len, rx->cap, "<group>%s</group>", groups[i]) != 0) {
      rx->error = 1;
      return;
    }
  }
  if (iq_append(rx->buf, &rx->len, rx->cap, "</item>") != 0) {
    rx->error = 1;
  }
}

static int roster_list_cb(const storage_roster_item_t* item, const char** groups, int gc,
                          void* ud) {
  roster_xml_ctx_t* rx = (roster_xml_ctx_t*)ud;
  append_item_xml(rx, item, groups, gc);
  return rx->error ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Roster handlers                                                   */
/* ------------------------------------------------------------------ */

/* Handle jabber:iq:roster get.
 * New signature: matches iq_handler_fn
 * Returns: IQ_HANDLED on success, IQ_NOT_MINE if wrong namespace/type.
 */
static iq_handler_result_t xmpp_iq_handle_roster_get(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                              xmpp_stanza_t* child, const char* iq_id) {
  (void)stanza;

  /* XEP-0059 §2.3: read optional ver= from incoming <query ver='...'>. */
  const char* req_ver = xmpp_stanza_get_attribute(child, "ver");

  /* Derive owner bare JID from authcid and domain. */
  char owner[2048];
  (void)snprintf(owner, sizeof(owner), "%s@%s", ctx->authcid, ctx->domain);

  /* XEP-0059 §2.4: if client sent a version, check for a match. */
  if (req_ver && req_ver[0]) {
    char stored_ver[ROSTER_VER_SIZE];
    int vr = roster_ver_get(owner, stored_ver);
    if (vr == 0 && strcmp(req_ver, stored_ver) == 0) {
      /* XEP-0059 §2.4: roster unchanged — return 304 / empty result with ver=. */
      char rbuf[512];
      size_t rlen = 0;
      if (iq_id) {
        (void)iq_append(rbuf, &rlen, sizeof(rbuf),
                       "<iq type='result' id='%s'>"
                       "<query xmlns='jabber:iq:roster' ver='%s'/>"
                       "</iq>",
                       iq_id, stored_ver);
      } else {
        (void)iq_append(rbuf, &rlen, sizeof(rbuf),
                       "<iq type='result'>"
                       "<query xmlns='jabber:iq:roster' ver='%s'/>"
                       "</iq>",
                       stored_ver);
      }
      iq_flush(ctx, rbuf, rlen);
      stump_d("roster get (ver=%s) unchanged for %s", req_ver, owner);
      return IQ_HANDLED;
    }
    /* vr == 1 (no version yet) or mismatch → fall through to send full roster. */
  }

  /* Fetch current version (will be computed if first-ever request). */
  char cur_ver[ROSTER_VER_SIZE];
  (void)roster_ver_peek(owner, cur_ver);
  cur_ver[0] = '\0'; /* safe default if peek fails */

  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("roster get: out of memory");
    return IQ_ERROR;
  }
  size_t len = 0;

  if (iq_id) {
    if (iq_append(buf, &len, IQ_BUF_SIZE,
                  "<iq type='result' id='%s'>"
                  "<query xmlns='jabber:iq:roster'%s%s%s>"
                  "</iq>",
                  iq_id,
                  cur_ver[0] ? " ver='" : "",
                  cur_ver[0] ? cur_ver : "",
                  cur_ver[0] ? "'" : "") != 0) {
      free(buf);
      return IQ_ERROR;
    }
  } else {
    if (iq_append(buf, &len, IQ_BUF_SIZE,
                  "<iq type='result'>"
                  "<query xmlns='jabber:iq:roster'%s%s%s>"
                  "</iq>",
                  cur_ver[0] ? " ver='" : "",
                  cur_ver[0] ? cur_ver : "",
                  cur_ver[0] ? "'" : "") != 0) {
      free(buf);
      return IQ_ERROR;
    }
  }

  roster_xml_ctx_t rx = {buf, len, IQ_BUF_SIZE, 0};
  rx.len = len;

  if (storage_roster_list(owner, roster_list_cb, &rx) != 0 || rx.error) {
    len = 0;
    if (iq_id) {
      iq_append(buf, &len, IQ_BUF_SIZE,
                "<iq type='error' id='%s'>"
                "<error type='cancel'>"
                "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>",
                iq_id);
    } else {
      iq_append(buf, &len, IQ_BUF_SIZE,
                "<iq type='error'>"
                "<error type='cancel'>"
                "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>");
    }
    iq_flush(ctx, buf, len);
    free(buf);
    return IQ_ERROR;
  }

  len = rx.len;
  if (iq_append(buf, &len, IQ_BUF_SIZE, "</query></iq>") != 0) {
    stump_er("roster get: buffer overflow for '%s'", owner);
    free(buf);
    return IQ_ERROR;
  }

  stump_d("roster get owner=%s iq_id=%s", owner, iq_id ? iq_id : "(none)");
  iq_flush(ctx, buf, len);
  free(buf);
  return IQ_HANDLED;
}

/* Handle jabber:iq:roster set (add/update/remove). */
static void send_roster_push(xmpp_session_t* ctx, const storage_roster_item_t* item,
                             const char** groups, int gc, const char* push_id,
                             const char* owner) {
  char buf[4096];
  size_t len = 0;

  /* XEP-0059 §2.2: include ver= attribute on every roster push. */
  char ver[ROSTER_VER_SIZE];
  ver[0] = '\0';
  (void)roster_ver_peek(owner, ver);

  if (iq_append(buf, &len, sizeof(buf),
                "<iq type='set' id='%s'>"
                "<query xmlns='jabber:iq:roster'%s%s%s>",
                push_id,
                ver[0] ? " ver='" : "",
                ver[0] ? ver : "",
                ver[0] ? "'" : "") != 0) {
    return;
  }
  roster_xml_ctx_t rx = {buf, len, sizeof(buf), 0};
  append_item_xml(&rx, item, groups, gc);
  len = rx.len;
  if (iq_append(buf, &len, sizeof(buf), "</query></iq>") != 0) {
    return;
  }
  iq_flush(ctx, buf, len);
}

static iq_handler_result_t xmpp_iq_handle_roster_set(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                              xmpp_stanza_t* child, const char* iq_id) {
  (void)stanza;

  char owner[2048];
  (void)snprintf(owner, sizeof(owner), "%s@%s", ctx->authcid, ctx->domain);

  /* RFC 6121 §2.1.5: <query> MUST contain exactly one <item>. */
  xmpp_stanza_t* item_el = xmpp_stanza_get_child_by_name(child, "item");
  if (!item_el) {
    send_iq_error(ctx, iq_id, "modify", "bad-request");
    return IQ_ERROR;
  }

  /* Check for multiple items — reject with bad-request. */
  xmpp_stanza_t* next = xmpp_stanza_get_next(item_el);
  while (next) {
    if (strcmp(xmpp_stanza_get_name(next), "item") == 0) {
      send_iq_error(ctx, iq_id, "modify", "bad-request");
      return IQ_ERROR;
    }
    next = xmpp_stanza_get_next(next);
  }

  const char* contact_jid = xmpp_stanza_get_attribute(item_el, "jid");
  if (!contact_jid || contact_jid[0] == '\0') {
    send_iq_error(ctx, iq_id, "modify", "bad-request");
    return IQ_ERROR;
  }

  const char* sub_attr = xmpp_stanza_get_attribute(item_el, "subscription");
  int is_remove = (sub_attr && strcmp(sub_attr, "remove") == 0);

  if (is_remove) {
    if (storage_roster_remove(owner, contact_jid) != 0) {
      send_iq_error(ctx, iq_id, "cancel", "internal-server-error");
      return IQ_ERROR;
    }

    send_iq_result(ctx, iq_id);

    char push_id[32];
    (void)snprintf(push_id, sizeof(push_id), "push-%s", iq_id ? iq_id : "r");
    storage_roster_item_t push_item;
    memset(&push_item, 0, sizeof(push_item));
    (void)snprintf(push_item.contact_jid, sizeof(push_item.contact_jid), "%s", contact_jid);
    (void)snprintf(push_item.subscription, sizeof(push_item.subscription), "%s", "remove");
    send_roster_push(ctx, &push_item, NULL, 0, push_id, owner);

    stump_d("roster remove owner=%s contact=%s", owner, contact_jid);
    return IQ_HANDLED;
  }

  const char* name_attr = xmpp_stanza_get_attribute(item_el, "name");

  const char* groups[32];
  char* group_allocs[32];
  int gc = 0;
  xmpp_stanza_t* child_el = xmpp_stanza_get_children(item_el);
  while (child_el && gc < 32) {
    if (strcmp(xmpp_stanza_get_name(child_el), "group") == 0) {
      char* gtext = xmpp_stanza_get_text(child_el);
      if (gtext && gtext[0] != '\0') {
        groups[gc] = gtext;
        group_allocs[gc] = gtext;
        gc++;
      } else {
        free(gtext);
      }
    }
    child_el = xmpp_stanza_get_next(child_el);
  }

  storage_roster_item_t item;
  memset(&item, 0, sizeof(item));
  (void)snprintf(item.contact_jid, sizeof(item.contact_jid), "%s", contact_jid);
  if (name_attr) {
    (void)snprintf(item.name, sizeof(item.name), "%s", name_attr);
  }
  (void)snprintf(item.subscription, sizeof(item.subscription), "%s", "none");

  if (storage_roster_upsert(owner, &item, groups, gc) != 0) {
    for (int i = 0; i < gc; i++) {
      free(group_allocs[i]);
    }
    send_iq_error(ctx, iq_id, "cancel", "internal-server-error");
    return IQ_ERROR;
  }

  send_iq_result(ctx, iq_id);

  char push_id[32];
  (void)snprintf(push_id, sizeof(push_id), "push-%s", iq_id ? iq_id : "r");
  send_roster_push(ctx, &item, groups, gc, push_id, owner);

  for (int i = 0; i < gc; i++) {
    free(group_allocs[i]);
  }

  stump_d("roster set owner=%s contact=%s", owner, contact_jid);
  return IQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/* Legacy compatibility wrapper — delegates to iq_dispatch(). */
void xmpp_iq_dispatch(xmpp_session_t* ctx, xmpp_stanza_t* stanza) {
  iq_dispatch(ctx, stanza);
}

/* Roster handlers for registration. */
static const iq_handler_entry_t roster_handlers[] = {
    IQ_HANDLER("jabber:iq:roster", "get",  IQ_PRIORITY_NORMAL, xmpp_iq_handle_roster_get),
    IQ_HANDLER("jabber:iq:roster", "set",  IQ_PRIORITY_NORMAL, xmpp_iq_handle_roster_set),
    IQ_HANDLERS_END
};

int xmpp_iq_register_handlers(void) {
    return iq_handler_register_all(roster_handlers);
}
