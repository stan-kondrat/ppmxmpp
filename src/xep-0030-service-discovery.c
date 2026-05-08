#include "xep-0030-service-discovery.h"

#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "storage/users.h"
#include "stumpless.h"
#include "xmpp_iq_buf.h"

static const char* server_features[] = {
    "urn:xmpp:ping",
    "http://jabber.org/protocol/disco#info",
    "jabber:iq:roster",
    NULL,
};

typedef enum {
  TARGET_SERVER,        /* server domain or no 'to' */
  TARGET_LOCAL_USER,    /* existing bare JID on this server */
  TARGET_LOCAL_MISSING, /* bare JID on this server but user not found */
  TARGET_UNKNOWN,       /* foreign domain or other unknown entity */
} disco_target_t;

static void send_error(xmpp_session_t* ctx, const char* iq_id, const char* condition) {
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

/* XEP-0030 §3.1 + Security Considerations:
 * Returns TARGET_SERVER, TARGET_LOCAL_USER, or TARGET_UNKNOWN. */
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
    /* XEP-0030 Security Considerations: non-existent bare JID on this server
     * → service-unavailable (prevents directory harvesting). */
    return TARGET_LOCAL_MISSING;
  }
  return TARGET_UNKNOWN;
}

void xep0030_handle_disco_info(xmpp_session_t* ctx, const char* iq_id, const char* to,
                               const char* node) {
  disco_target_t target = disco_resolve_target(ctx, to);

  if (target == TARGET_LOCAL_MISSING) {
    /* Non-existent bare JID on this server: service-unavailable per
     * XEP-0030 Security Considerations (prevents directory harvesting). */
    send_error(ctx, iq_id, "service-unavailable");
    return;
  }

  if (target == TARGET_UNKNOWN) {
    send_error(ctx, iq_id, "item-not-found");
    return;
  }

  /* R4: this server has no named nodes — unknown node → item-not-found. */
  if (node && node[0] != '\0') {
    send_error(ctx, iq_id, "item-not-found");
    return;
  }

  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("disco#info: out of memory");
    return;
  }
  size_t len = 0;
  int rc;

  if (iq_id) {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='result' id='%s'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'>",
                   iq_id);
  } else {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='result'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'>");
  }
  if (rc != 0) goto overflow;

  /* XEP-0030 §3.1: bare JID result uses category='account' type='registered';
   * server domain uses category='server' type='im'. */
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
  return;

overflow:
  stump_er("disco#info: response buffer overflow");
  free(buf);
}
