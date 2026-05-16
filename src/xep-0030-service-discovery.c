/* XEP-0030: Service Discovery
 * https://xmpp.org/extensions/xep-0030.html
 *
 * §3.1  disco#info: return identity + feature list for a JID or server.
 * §3.2  disco#items: return child items (components, rooms, etc.).
 * Security Considerations: non-existent local bare JID → service-unavailable
 *   (prevents user enumeration).
 */
#include "xep-0030-service-discovery.h"

#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "storage/db_users.h"
#include "strophe.h"
#include "log.h"
#include "xmpp_iq_buf.h"
#include "xmpp_iq_dispatch.h"

static const char* server_features[] = {
    "urn:xmpp:ping",
    "urn:xmpp:carbons:2",
    "urn:xmpp:forward:0",
    "http://jabber.org/protocol/disco#info",
    "http://jabber.org/protocol/disco#items",
    "jabber:iq:roster",
    NULL,
};

typedef enum {
  TARGET_SERVER,        /* server domain or no 'to' */
  TARGET_LOCAL_USER,    /* existing bare JID on this server */
  TARGET_LOCAL_MISSING, /* bare JID on this server but user not found */
  TARGET_UNKNOWN,       /* foreign domain or other unknown entity */
} disco_target_t;

static void send_info_error(xmpp_session_t* ctx, const char* iq_id, const char* condition) {
  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("disco#info: out of memory (error response)");
    return;
  }
  size_t len = 0;
  int rc;
  if (iq_id) {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='error' id='%s'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/>"
                   "<error type='cancel'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   iq_id, condition);
  } else {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='error'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/>"
                   "<error type='cancel'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   condition);
  }
  if (rc == 0) {
    iq_flush(ctx, buf, len);
  } else {
    stump_er("disco#info: error stanza too large");
  }
  free(buf);
}

static void send_items_error(xmpp_session_t* ctx, const char* iq_id, const char* condition) {
  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("disco#items: out of memory (error response)");
    return;
  }
  size_t len = 0;
  int rc;
  if (iq_id) {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='error' id='%s'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/>"
                   "<error type='cancel'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   iq_id, condition);
  } else {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='error'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/>"
                   "<error type='cancel'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   condition);
  }
  if (rc == 0) {
    iq_flush(ctx, buf, len);
  } else {
    stump_er("disco#items: error stanza too large");
  }
  free(buf);
}

/* XEP-0030 §3.1, Security Considerations: classify the 'to' attribute. */
static disco_target_t disco_resolve_target(const xmpp_session_t* ctx, const char* to) {
  if (!to || strcmp(to, ctx->domain) == 0) {
    return TARGET_SERVER;
  }
  const char* at = strchr(to, '@');
  if (at && strcmp(at + 1, ctx->domain) == 0) {
    sqlite3* db;
    if (storage_db_open(&db) != 0) {
      return TARGET_UNKNOWN;
    }
    storage_user_t u;
    int found = storage_users_get_by_jid(to, &u);
    storage_db_close();
    if (found == 0) {
      return TARGET_LOCAL_USER;
    }
    /* XEP-0030 Security Considerations: return service-unavailable, not item-not-found,
     * so the response is indistinguishable from a real user's (prevents enumeration). */
    return TARGET_LOCAL_MISSING;
  }
  return TARGET_UNKNOWN;
}

/* XEP-0030 §3.1: handle disco#info get IQ. */
static iq_handler_result_t xep0030_handle_disco_info_iq(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                                xmpp_stanza_t* child, const char* iq_id) {
  const char* to = xmpp_stanza_get_attribute(stanza, "to");
  const char* node = child ? xmpp_stanza_get_attribute(child, "node") : NULL;

  disco_target_t target = disco_resolve_target(ctx, to);

  if (target == TARGET_LOCAL_MISSING) {
    send_info_error(ctx, iq_id, "service-unavailable");
    return IQ_ERROR;
  }

  if (target == TARGET_UNKNOWN) {
    send_info_error(ctx, iq_id, "item-not-found");
    return IQ_ERROR;
  }

  if (node && node[0] != '\0') {
    send_info_error(ctx, iq_id, "item-not-found");
    return IQ_ERROR;
  }

  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("disco#info: out of memory");
    return IQ_ERROR;
  }
  size_t len = 0;
  int rc;

  const char* from_jid = (to && to[0]) ? to : ctx->domain;

  if (iq_id) {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='result' id='%s' from='%s'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'>",
                   iq_id, from_jid);
  } else {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='result' from='%s'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'>",
                   from_jid);
  }
  if (rc != 0) goto overflow;

  /* XEP-0030 §3.1: bare JID → category='account' type='registered';
   * server domain → category='server' type='im'. */
  if (target == TARGET_LOCAL_USER) {
    if (iq_append(buf, &len, IQ_BUF_SIZE,
                  "<identity category='account' type='registered'/>") != 0) goto overflow;
  } else {
    if (iq_append(buf, &len, IQ_BUF_SIZE,
                  "<identity category='server' name='PPMXMPP' type='im'/>") != 0) goto overflow;
  }

  for (int i = 0; server_features[i]; i++) {
    if (iq_append(buf, &len, IQ_BUF_SIZE, "<feature var='%s'/>", server_features[i]) != 0)
      goto overflow;
  }

  if (iq_append(buf, &len, IQ_BUF_SIZE, "</query></iq>") != 0) goto overflow;

  iq_flush(ctx, buf, len);
  free(buf);
  return IQ_HANDLED;

overflow:
  stump_er("disco#info: response buffer overflow");
  free(buf);
  return IQ_ERROR;
}

/* XEP-0030 §3.2: handle disco#items get IQ. Server has no child items. */
static iq_handler_result_t xep0030_handle_disco_items_iq(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                                   xmpp_stanza_t* child, const char* iq_id) {
  const char* to = xmpp_stanza_get_attribute(stanza, "to");
  const char* node = child ? xmpp_stanza_get_attribute(child, "node") : NULL;

  disco_target_t target = disco_resolve_target(ctx, to);

  if (target == TARGET_LOCAL_MISSING) {
    send_items_error(ctx, iq_id, "service-unavailable");
    return IQ_ERROR;
  }

  if (target == TARGET_UNKNOWN) {
    send_items_error(ctx, iq_id, "item-not-found");
    return IQ_ERROR;
  }

  if (node && node[0] != '\0') {
    send_items_error(ctx, iq_id, "item-not-found");
    return IQ_ERROR;
  }

  char buf[256];
  size_t len = 0;
  const char* from_jid = (to && to[0]) ? to : ctx->domain;

  int rc;
  if (iq_id) {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<iq type='result' id='%s' from='%s'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/>"
                   "</iq>",
                   iq_id, from_jid);
  } else {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<iq type='result' from='%s'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/>"
                   "</iq>",
                   from_jid);
  }

  if (rc != 0) {
    stump_er("disco#items: response too large");
    return IQ_ERROR;
  }

  iq_flush(ctx, buf, len);
  return IQ_HANDLED;
}

/* Handler registration table. */
static const iq_handler_entry_t disco_handlers[] = {
    IQ_HANDLER("http://jabber.org/protocol/disco#info",  "get", IQ_PRIORITY_NORMAL,
               xep0030_handle_disco_info_iq),
    IQ_HANDLER("http://jabber.org/protocol/disco#items", "get", IQ_PRIORITY_NORMAL,
               xep0030_handle_disco_items_iq),
    IQ_HANDLERS_END
};

int xep0030_init(void) {
    return iq_handler_register_all(disco_handlers);
}

