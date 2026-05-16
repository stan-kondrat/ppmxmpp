#include "xep-0160-offline-messages.h"

#include "storage/db_offline.h"
#include "stumpless.h"
#include "xmpp_iq_buf.h"

/* Send a service-unavailable error back to the sender. */
static void send_cap_error(xmpp_session_t* ctx, const char* msg_id) {
  char buf[512];
  size_t len = 0;
  int rc;
  if (msg_id) {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<message type='error' id='%s' to='%s'>"
                   "<error type='cancel'>"
                   "<service-unavailable xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error>"
                   "</message>",
                   msg_id, ctx->bound_jid);
  } else {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<message type='error' to='%s'>"
                   "<error type='cancel'>"
                   "<service-unavailable xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error>"
                   "</message>",
                   ctx->bound_jid);
  }
  if (rc == 0) ctx->write_fn(ctx->write_ud, buf, len);
}

int xep0160_store(xmpp_session_t* ctx, const char* to_bare, const char* msg_id,
                  const char* stanza_xml, size_t stanza_len) {
  int rc = offline_store(to_bare, ctx->bound_jid, stanza_xml, stanza_len);
  if (rc == -2) {
    send_cap_error(ctx, msg_id);
    stump_w("xep-0160: offline storage cap reached for %s, rejecting message", to_bare);
  } else if (rc != 0) {
    stump_er("xep-0160: failed to store offline message for %s", to_bare);
  } else {
    stump_i("xep-0160: stored offline for %s from %s", to_bare, ctx->bound_jid);
  }
  return rc;
}
