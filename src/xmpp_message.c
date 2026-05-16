#include "xmpp_message.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "strophe.h"
#include "stumpless.h"
#include "xep-0160-offline-messages.h"
#include "xmpp_iq_buf.h"
#include "xmpp_session.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Strip resource from a full JID, writing bare JID into out. */
static void msg_bare_jid(const char* full, char* out, size_t out_size) {
  const char* slash = strchr(full, '/');
  size_t len = slash ? (size_t)(slash - full) : strlen(full);
  if (len >= out_size) len = out_size - 1;
  memcpy(out, full, len);
  out[len] = '\0';
}

/* Return a pointer to the resource part of a full JID, or NULL if bare. */
static const char* msg_resource(const char* full) {
  const char* slash = strchr(full, '/');
  return slash ? slash + 1 : NULL;
}

/* Extract the domain from a JID (bare or full). */
static void msg_domain(const char* jid, char* out, size_t out_size) {
  const char* at = strchr(jid, '@');
  const char* start = at ? at + 1 : jid;
  const char* slash = strchr(start, '/');
  size_t len = slash ? (size_t)(slash - start) : strlen(start);
  if (len >= out_size) len = out_size - 1;
  memcpy(out, start, len);
  out[len] = '\0';
}

/* Build the forwarded message stanza into buf.
 * Copies type, id, from, to, and <body> from the incoming stanza. */
static int build_message(char* buf, size_t* len, size_t cap, const char* from_jid,
                         const char* to_jid, const char* msg_type, const char* msg_id,
                         const char* body_text) {
  int rc;
  if (msg_type && msg_id) {
    rc = iq_append(buf, len, cap, "<message from='%s' to='%s' type='%s' id='%s'>", from_jid,
                   to_jid, msg_type, msg_id);
  } else if (msg_type) {
    rc = iq_append(buf, len, cap, "<message from='%s' to='%s' type='%s'>", from_jid, to_jid,
                   msg_type);
  } else if (msg_id) {
    rc = iq_append(buf, len, cap, "<message from='%s' to='%s' id='%s'>", from_jid, to_jid, msg_id);
  } else {
    rc = iq_append(buf, len, cap, "<message from='%s' to='%s'>", from_jid, to_jid);
  }
  if (rc != 0) {
    stump_er("message: build_message buffer overflow (from=%s to=%s)", from_jid, to_jid);
    return -1;
  }

  if (body_text && body_text[0]) {
    if (iq_append(buf, len, cap, "<body>%s</body>", body_text) != 0) {
      stump_er("message: build_message buffer overflow on body (from=%s)", from_jid);
      return -1;
    }
  }

  if (iq_append(buf, len, cap, "</message>") != 0) {
    stump_er("message: build_message buffer overflow on closing tag (from=%s)", from_jid);
    return -1;
  }
  return 0;
}

/* Send a stanza-level error back to the sender. */
static void send_message_error(xmpp_session_t* ctx, const char* to_attr, const char* msg_id,
                               const char* error_type, const char* condition_ns,
                               const char* condition) {
  char buf[512];
  size_t len = 0;
  int rc;
  if (msg_id) {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<message type='error' id='%s' to='%s'>"
                   "<error type='%s'>"
                   "<%s xmlns='%s'/>"
                   "</error>"
                   "</message>",
                   msg_id, to_attr ? to_attr : "", error_type, condition, condition_ns);
  } else {
    rc = iq_append(buf, &len, sizeof(buf),
                   "<message type='error' to='%s'>"
                   "<error type='%s'>"
                   "<%s xmlns='%s'/>"
                   "</error>"
                   "</message>",
                   to_attr ? to_attr : "", error_type, condition, condition_ns);
  }
  if (rc == 0) ctx->write_fn(ctx->write_ud, buf, len);
}

/* ------------------------------------------------------------------ */
/*  Message handler                                                    */
/* ------------------------------------------------------------------ */

void xmpp_message_handle(xmpp_session_t* ctx, xmpp_stanza_t* stanza) {
  const char* to_attr  = xmpp_stanza_get_attribute(stanza, "to");
  const char* msg_type = xmpp_stanza_get_attribute(stanza, "type");
  const char* msg_id   = xmpp_stanza_get_attribute(stanza, "id");

  stump_d("message: from=%s to=%s type=%s", ctx->bound_jid, to_attr ? to_attr : "(none)",
          msg_type ? msg_type : "(none)");

  /* RFC 6121 §5.2.2: to= is required for server routing. */
  if (!to_attr || to_attr[0] == '\0') {
    send_message_error(ctx, NULL, msg_id, "modify",
                       "urn:ietf:params:xml:ns:xmpp-stanzas", "bad-request");
    return;
  }

  /* Only route within this server's domain. */
  char to_domain[1024];
  msg_domain(to_attr, to_domain, sizeof(to_domain));
  if (strcmp(to_domain, ctx->domain) != 0) {
    send_message_error(ctx, to_attr, msg_id, "cancel",
                       "urn:ietf:params:xml:ns:xmpp-stanzas", "service-unavailable");
    return;
  }

  /* Extract <body> text (allocated by libstrophe — must free). */
  xmpp_stanza_t* body_el = xmpp_stanza_get_child_by_name(stanza, "body");
  char* body_text = body_el ? xmpp_stanza_get_text(body_el) : NULL;

  char to_bare[3073];
  msg_bare_jid(to_attr, to_bare, sizeof(to_bare));
  const char* to_resource = msg_resource(to_attr);

  char fwd[IQ_BUF_SIZE];
  size_t fwd_len = 0;

  if (to_resource) {
    /* Full JID target: try exact resource first, fall back to bare-JID routing. */
    if (build_message(fwd, &fwd_len, sizeof(fwd), ctx->bound_jid, to_attr, msg_type, msg_id,
                      body_text) == 0) {
      if (xmpp_session_table_write(to_attr, fwd, fwd_len) == 0) {
        xmpp_session_table_touch(ctx->bound_jid);
        free(body_text);
        return;
      }
    }
    /* Fall through to bare-JID routing below. */
    fwd_len = 0;
  }

  /* Bare JID routing. */
  char sender_bare[3073];
  msg_bare_jid(ctx->bound_jid, sender_bare, sizeof(sender_bare));

  if (strcmp(to_bare, sender_bare) == 0) {
    /* Self-addressed: deliver to all other resources of this user. */
    if (build_message(fwd, &fwd_len, sizeof(fwd), ctx->bound_jid, to_bare, msg_type, msg_id,
                      body_text) == 0) {
      xmpp_session_table_broadcast_except(sender_bare, ctx->bound_jid, fwd, fwd_len);
      xmpp_session_table_touch(ctx->bound_jid);
    }
  } else {
    /* Normal bare-JID routing: pick best resource. */
    char best_jid[3073];
    if (xmpp_session_table_best_resource(to_bare, best_jid, sizeof(best_jid)) == 0) {
      if (build_message(fwd, &fwd_len, sizeof(fwd), ctx->bound_jid, best_jid, msg_type, msg_id,
                        body_text) == 0) {
        xmpp_session_table_write(best_jid, fwd, fwd_len);
        xmpp_session_table_touch(ctx->bound_jid);
      }
    } else {
      /* No online resource: store for offline delivery (XEP-0160). */
      stump_d("message: %s is offline, storing for later delivery", to_bare);
      if (build_message(fwd, &fwd_len, sizeof(fwd), ctx->bound_jid, to_bare, msg_type, msg_id,
                        body_text) == 0) {
        xep0160_store(ctx, to_bare, msg_id, fwd, fwd_len);
        xmpp_session_table_touch(ctx->bound_jid);
      }
    }
  }

  free(body_text);
}
