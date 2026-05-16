#include "xmpp_iq.h"

#include <stdlib.h>
#include <string.h>

#include "storage/db_roster.h"
#include "strophe.h"
#include "stumpless.h"
#include "xep-0030-service-discovery.h"
#include "xep-0199-ping.h"
#include "xep-0280-carbons.h"
#include "xmpp_iq_buf.h"

/* ------------------------------------------------------------------ */
/*  Roster XML serialisation helpers                                   */
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
/*  jabber:iq:roster get                                               */
/* ------------------------------------------------------------------ */

static void handle_roster_get(xmpp_session_t* ctx, const char* iq_id) {
  /* Derive owner bare JID from authcid and domain. */
  char owner[2048];
  snprintf(owner, sizeof(owner), "%s@%s", ctx->authcid, ctx->domain);

  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("roster get: out of memory");
    return;
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
  /* Point len into rx so the callbacks advance it. */
  rx.len = len;

  if (storage_roster_list(owner, roster_list_cb, &rx) != 0 || rx.error) {
    /* Return internal-server-error */
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
    return;
  }

  len = rx.len;
  if (iq_append(buf, &len, IQ_BUF_SIZE, "</query></iq>") != 0) {
    goto overflow;
  }

  stump_d("roster get owner=%s iq_id=%s", owner, iq_id ? iq_id : "(none)");
  iq_flush(ctx, buf, len);
  free(buf);
  return;

overflow:
  stump_er("roster get: response buffer overflow for '%s'", owner);
  free(buf);
}

/* ------------------------------------------------------------------ */
/*  jabber:iq:roster set (add/update/remove)                          */
/* ------------------------------------------------------------------ */

/* Build and send a roster push to the requesting resource. */
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

static void handle_roster_set(xmpp_session_t* ctx, xmpp_stanza_t* query, const char* iq_id) {
  char owner[2048];
  snprintf(owner, sizeof(owner), "%s@%s", ctx->authcid, ctx->domain);

  /* RFC 6121 §2.1.5: <query> MUST contain exactly one <item>. */
  xmpp_stanza_t* item_el = xmpp_stanza_get_child_by_name(query, "item");
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
    return;
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
      return;
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
    return;
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
      return;
    }

    /* Acknowledge the set. */
    char ack[256];
    size_t ack_len = 0;
    if (iq_id) {
      iq_append(ack, &ack_len, sizeof(ack), "<iq type='result' id='%s'/>", iq_id);
    } else {
      iq_append(ack, &ack_len, sizeof(ack), "<iq type='result'/>");
    }
    iq_flush(ctx, ack, ack_len);

    /* Roster push with subscription='remove'. */
    char push_id[32];
    snprintf(push_id, sizeof(push_id), "push-%s", iq_id ? iq_id : "r");
    storage_roster_item_t push_item;
    memset(&push_item, 0, sizeof(push_item));
    strncpy(push_item.contact_jid, contact_jid, sizeof(push_item.contact_jid) - 1);
    strncpy(push_item.subscription, "remove", sizeof(push_item.subscription) - 1);
    send_roster_push(ctx, &push_item, NULL, 0, push_id);

    stump_d("roster remove owner=%s contact=%s", owner, contact_jid);
    return;
  }

  /* Add or update. */
  const char* name_attr = xmpp_stanza_get_attribute(item_el, "name");

  /* Collect <group> children. */
  const char* groups[32];
  char* group_allocs[32];
  int gc = 0;
  xmpp_stanza_t* child = xmpp_stanza_get_children(item_el);
  while (child && gc < 32) {
    if (strcmp(xmpp_stanza_get_name(child), "group") == 0) {
      char* gtext = xmpp_stanza_get_text(child);
      if (gtext && gtext[0] != '\0') {
        groups[gc] = gtext;
        group_allocs[gc] = gtext;
        gc++;
      } else {
        free(gtext);
      }
    }
    child = xmpp_stanza_get_next(child);
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
    return;
  }

  /* Acknowledge the set. */
  char ack[256];
  size_t ack_len = 0;
  if (iq_id) {
    iq_append(ack, &ack_len, sizeof(ack), "<iq type='result' id='%s'/>", iq_id);
  } else {
    iq_append(ack, &ack_len, sizeof(ack), "<iq type='result'/>");
  }
  iq_flush(ctx, ack, ack_len);

  /* Roster push. */
  char push_id[32];
  snprintf(push_id, sizeof(push_id), "push-%s", iq_id ? iq_id : "r");
  send_roster_push(ctx, &item, groups, gc, push_id);

  for (int i = 0; i < gc; i++) {
    free(group_allocs[i]);
  }

  stump_d("roster set owner=%s contact=%s", owner, contact_jid);
}

/* ------------------------------------------------------------------ */
/*  Public dispatcher                                                  */
/* ------------------------------------------------------------------ */

void xmpp_iq_dispatch(xmpp_session_t* ctx, xmpp_stanza_t* stanza) {
  const char* iq_type = xmpp_stanza_get_attribute(stanza, "type");
  const char* iq_id = xmpp_stanza_get_attribute(stanza, "id");

  if (!iq_type) {
    /* Malformed IQ — return bad-request. */
    char buf[256];
    size_t len = 0;
    iq_append(buf, &len, sizeof(buf),
              "<iq type='error'>"
              "<error type='modify'>"
              "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
              "</error></iq>");
    iq_flush(ctx, buf, len);
    return;
  }

  /* Find the first child element to key dispatch on its namespace. */
  xmpp_stanza_t* child = xmpp_stanza_get_children(stanza);
  /* Skip text nodes — get first element child. */
  while (child) {
    const char* ct = xmpp_stanza_get_type(child);
    /* Element nodes have type NULL or "tag"; text nodes have type "text". */
    if (!ct || strcmp(ct, "text") != 0) {
      break;
    }
    child = xmpp_stanza_get_next(child);
  }

  const char* ns = child ? xmpp_stanza_get_ns(child) : NULL;

  /* jabber:iq:roster */
  if (ns && strcmp(ns, "jabber:iq:roster") == 0) {
    if (strcmp(iq_type, "get") == 0) {
      handle_roster_get(ctx, iq_id);
      return;
    }
    if (strcmp(iq_type, "set") == 0) {
      handle_roster_set(ctx, child, iq_id);
      return;
    }
  }

  /* XEP-0030: Service Discovery — info */
  if (ns && strcmp(ns, "http://jabber.org/protocol/disco#info") == 0 &&
      strcmp(iq_type, "get") == 0) {
    const char* to = xmpp_stanza_get_attribute(stanza, "to");
    const char* node = xmpp_stanza_get_attribute(child, "node");
    xep0030_handle_disco_info(ctx, iq_id, to, node);
    return;
  }

  /* XEP-0030: Service Discovery — items (empty: no components or MUC rooms) */
  if (ns && strcmp(ns, "http://jabber.org/protocol/disco#items") == 0 &&
      strcmp(iq_type, "get") == 0) {
    char buf[256];
    size_t len = 0;
    const char* to_attr = xmpp_stanza_get_attribute(stanza, "to");
    const char* from_jid = (to_attr && to_attr[0]) ? to_attr : ctx->domain;
    if (iq_id) {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='result' id='%s' from='%s'>"
                "<query xmlns='http://jabber.org/protocol/disco#items'/>"
                "</iq>",
                iq_id, from_jid);
    } else {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='result' from='%s'>"
                "<query xmlns='http://jabber.org/protocol/disco#items'/>"
                "</iq>",
                from_jid);
    }
    iq_flush(ctx, buf, len);
    return;
  }

  /* XEP-0280: Message Carbons — enable/disable per-resource */
  if (ns && strcmp(ns, "urn:xmpp:carbons:2") == 0 && strcmp(iq_type, "set") == 0) {
    /* Find the <enable/> or <disable/> child element. */
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
    return;
  }

  /* XEP-0199: XMPP Ping */
  if (ns && strcmp(ns, "urn:xmpp:ping") == 0 && strcmp(iq_type, "get") == 0) {
    xep0199_handle_ping(ctx, iq_id);
    return;
  }

  /* Unsupported or result/error from client — return feature-not-implemented
   * only for get/set; ignore result/error per RFC 6120 §8.2.3. */
  if (strcmp(iq_type, "get") == 0 || strcmp(iq_type, "set") == 0) {
    char buf[512];
    size_t len = 0;
    if (iq_id) {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error' id='%s'>"
                "<error type='cancel'>"
                "<feature-not-implemented xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>",
                iq_id);
    } else {
      iq_append(buf, &len, sizeof(buf),
                "<iq type='error'>"
                "<error type='cancel'>"
                "<feature-not-implemented xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                "</error></iq>");
    }
    iq_flush(ctx, buf, len);
  }
}
