#include "xmpp_iq.h"

#include <stdlib.h>
#include <string.h>

#include "storage/db_roster.h"
#include "strophe.h"
#include "stumpless.h"
#include "xmpp_iq_buf.h"

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
  (void)child;

  /* Derive owner bare JID from authcid and domain. */
  char owner[2048];
  snprintf(owner, sizeof(owner), "%s@%s", ctx->authcid, ctx->domain);

  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("roster get: out of memory");
    return IQ_ERROR;
  }
  size_t len = 0;

  if (iq_id) {
    if (iq_append(buf, &len, IQ_BUF_SIZE,
                  "<iq type='result' id='%s'>"
                  "<query xmlns='jabber:iq:roster'>",
                  iq_id) != 0) {
      goto overflow;
    }
  } else {
    if (iq_append(buf, &len, IQ_BUF_SIZE,
                  "<iq type='result'>"
                  "<query xmlns='jabber:iq:roster'>") != 0) {
      goto overflow;
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
    goto overflow;
  }

  stump_d("roster get owner=%s iq_id=%s", owner, iq_id ? iq_id : "(none)");
  iq_flush(ctx, buf, len);
  free(buf);
  return IQ_HANDLED;

overflow:
  stump_er("roster get: response buffer overflow for '%s'", owner);
  free(buf);
  return IQ_ERROR;
}

/* Handle jabber:iq:roster set (add/update/remove). */
static void send_roster_push(xmpp_session_t* ctx, const storage_roster_item_t* item,
                             const char** groups, int gc, const char* push_id) {
  char buf[4096];
  size_t len = 0;
  iq_append(buf, &len, sizeof(buf),
            "<iq type='set' id='%s'>"
            "<query xmlns='jabber:iq:roster'>",
            push_id);
  roster_xml_ctx_t rx = {buf, len, sizeof(buf), 0};
  append_item_xml(&rx, item, groups, gc);
  len = rx.len;
  iq_append(buf, &len, sizeof(buf), "</query></iq>");
  iq_flush(ctx, buf, len);
}

static iq_handler_result_t xmpp_iq_handle_roster_set(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                              xmpp_stanza_t* child, const char* iq_id) {
  (void)stanza;

  char owner[2048];
  snprintf(owner, sizeof(owner), "%s@%s", ctx->authcid, ctx->domain);

  /* RFC 6121 §2.1.5: <query> MUST contain exactly one <item>. */
  xmpp_stanza_t* item_el = xmpp_stanza_get_child_by_name(child, "item");
  if (!item_el) {
    char buf[512];
    size_t len = 0;
    if (iq_id) {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error' id='%s'>"
                "<error type='modify'>"
                "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>",
                iq_id);
    } else {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error'>"
                "<error type='modify'>"
                "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>");
    }
    iq_flush(ctx, buf, len);
    return IQ_ERROR;
  }

  /* Check for multiple items — reject with bad-request. */
  xmpp_stanza_t* next = xmpp_stanza_get_next(item_el);
  while (next) {
    if (strcmp(xmpp_stanza_get_name(next), "item") == 0) {
      char buf[512];
      size_t len = 0;
      if (iq_id) {
        iq_append(buf, &len, sizeof(buf),
                  "<iq type='error' id='%s'>"
                  "<error type='modify'>"
                  "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                  "</error></iq>",
                  iq_id);
      } else {
        iq_append(buf, &len, sizeof(buf),
                  "<iq type='error'>"
                  "<error type='modify'>"
                  "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                  "</error></iq>");
      }
      iq_flush(ctx, buf, len);
      return IQ_ERROR;
    }
    next = xmpp_stanza_get_next(next);
  }

  const char* contact_jid = xmpp_stanza_get_attribute(item_el, "jid");
  if (!contact_jid || contact_jid[0] == '\0') {
    char buf[512];
    size_t len = 0;
    if (iq_id) {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error' id='%s'>"
                "<error type='modify'>"
                "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>",
                iq_id);
    } else {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error'>"
                "<error type='modify'>"
                "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>");
    }
    iq_flush(ctx, buf, len);
    return IQ_ERROR;
  }

  const char* sub_attr = xmpp_stanza_get_attribute(item_el, "subscription");
  int is_remove = (sub_attr && strcmp(sub_attr, "remove") == 0);

  if (is_remove) {
    if (storage_roster_remove(owner, contact_jid) != 0) {
      char buf[512];
      size_t len = 0;
      if (iq_id) {
        iq_append(buf, &len, sizeof(buf),
                  "<iq type='error' id='%s'>"
                  "<error type='cancel'>"
                  "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                  "</error></iq>",
                  iq_id);
      } else {
        iq_append(buf, &len, sizeof(buf),
                  "<iq type='error'>"
                  "<error type='cancel'>"
                  "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                  "</error></iq>");
      }
      iq_flush(ctx, buf, len);
      return IQ_ERROR;
    }

    char ack[256];
    size_t ack_len = 0;
    if (iq_id) {
      iq_append(ack, &ack_len, sizeof(ack), "<iq type='result' id='%s'/>", iq_id);
    } else {
      iq_append(ack, &ack_len, sizeof(ack), "<iq type='result'/>");
    }
    iq_flush(ctx, ack, ack_len);

    char push_id[32];
    snprintf(push_id, sizeof(push_id), "push-%s", iq_id ? iq_id : "r");
    storage_roster_item_t push_item;
    memset(&push_item, 0, sizeof(push_item));
    strncpy(push_item.contact_jid, contact_jid, sizeof(push_item.contact_jid) - 1);
    strncpy(push_item.subscription, "remove", sizeof(push_item.subscription) - 1);
    send_roster_push(ctx, &push_item, NULL, 0, push_id);

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
  strncpy(item.contact_jid, contact_jid, sizeof(item.contact_jid) - 1);
  if (name_attr) {
    strncpy(item.name, name_attr, sizeof(item.name) - 1);
  }
  strncpy(item.subscription, "none", sizeof(item.subscription) - 1);

  if (storage_roster_upsert(owner, &item, groups, gc) != 0) {
    for (int i = 0; i < gc; i++) {
      free(group_allocs[i]);
    }
    char buf[512];
    size_t len = 0;
    if (iq_id) {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error' id='%s'>"
                "<error type='cancel'>"
                "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>",
                iq_id);
    } else {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error'>"
                "<error type='cancel'>"
                "<internal-server-error xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>");
    }
    iq_flush(ctx, buf, len);
    return IQ_ERROR;
  }

  char ack[256];
  size_t ack_len = 0;
  if (iq_id) {
    iq_append(ack, &ack_len, sizeof(ack), "<iq type='result' id='%s'/>", iq_id);
  } else {
    iq_append(ack, &ack_len, sizeof(ack), "<iq type='result'/>");
  }
  iq_flush(ctx, ack, ack_len);

  char push_id[32];
  snprintf(push_id, sizeof(push_id), "push-%s", iq_id ? iq_id : "r");
  send_roster_push(ctx, &item, groups, gc, push_id);

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
