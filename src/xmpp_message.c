#include "xmpp_message.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "strophe.h"
#include "log.h"
#include "xep-0160-offline-messages.h"
#include "xep-0280-carbons.h"
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

/* Check whether stanza contains <private xmlns='urn:xmpp:carbons:2'/> (XEP-0280 §9).
 * Returns 1 if the private element is present, 0 otherwise. */
static int msg_has_private(const xmpp_stanza_t* stanza) {
  xmpp_stanza_t* child = xmpp_stanza_get_children((xmpp_stanza_t*)stanza);
  while (child) {
    const char* name = xmpp_stanza_get_name(child);
    const char* ns = xmpp_stanza_get_ns(child);
    if (name && strcmp(name, "private") == 0 && ns &&
        strcmp(ns, "urn:xmpp:carbons:2") == 0) {
      return 1;
    }
    child = xmpp_stanza_get_next(child);
  }
  return 0;
}

/* Carbons copy data passed to the iterator callback. */
typedef struct {
  const char* sender_bare;      /* bare JID of the user whose messages are carbon-copied */
  const char* orig_from;         /* from= of the original stanza */
  const char* orig_to;           /* to= of the original stanza */
  const char* orig_type;         /* type= of the original stanza */
  const char* orig_id;           /* id= of the original stanza */
  const char* orig_body;         /* body text of the original stanza */
  const char* carbon_dir;        /* "sent" or "received" */
} carbon_copy_ctx_t;

/* Send one XEP-0297 carbon copy to a single resource.
 * Called by xmpp_session_table_for_each_carbon_resource. */
static void send_carbon_copy(const char* full_jid, xmpp_write_fn write_fn, void* write_ud,
                             const void* ud) {
  const carbon_copy_ctx_t* cc = (const carbon_copy_ctx_t*)ud;
  stump_d("carbons: callback for %s dir=%s from=%s", full_jid, cc->carbon_dir, cc->orig_from);

  /* Build XEP-0297 forwarded wrapper (XEP-0280 §4/§5):
   * <message from='sender_bare' to='full_jid'>
   *   <sent|received xmlns='urn:xmpp:carbons:2'>
   *     <forwarded xmlns='urn:xmpp:forward:0'>
   *       <original-stanza .../>
   *     </forwarded>
   *   </sent|received>
   * </message> */
  char buf[4096];
  size_t len = 0;

  /* Wrapper: from=sender_bare (XEP-0280 mandates this), to=full_jid,
   * type mirrors original (XEP-0280 §4/§5 recommendation). */
  if (cc->orig_type && cc->orig_type[0]) {
    iq_append(buf, &len, sizeof(buf),
              "<message from='%s' to='%s' type='%s'>"
              "<%s xmlns='urn:xmpp:carbons:2'>"
              "<forwarded xmlns='urn:xmpp:forward:0'>"
              "<message xmlns='jabber:client'",
              cc->sender_bare, full_jid, cc->orig_type, cc->carbon_dir);
  } else {
    iq_append(buf, &len, sizeof(buf),
              "<message from='%s' to='%s'>"
              "<%s xmlns='urn:xmpp:carbons:2'>"
              "<forwarded xmlns='urn:xmpp:forward:0'>"
              "<message xmlns='jabber:client'",
              cc->sender_bare, full_jid, cc->carbon_dir);
  }

  /* Original stanza attributes. */
  iq_append(buf, &len, sizeof(buf), " from='%s' to='%s'", cc->orig_from, cc->orig_to);
  if (cc->orig_type && cc->orig_type[0]) {
    iq_append(buf, &len, sizeof(buf), " type='%s'", cc->orig_type);
  }
  if (cc->orig_id && cc->orig_id[0]) {
    iq_append(buf, &len, sizeof(buf), " id='%s'", cc->orig_id);
  }
  iq_append(buf, &len, sizeof(buf), ">");

  /* Original body (and any other child elements could be added here). */
  if (cc->orig_body && cc->orig_body[0]) {
    iq_append(buf, &len, sizeof(buf), "<body>%s</body>", cc->orig_body);
  }

  iq_append(buf, &len, sizeof(buf),
            "</message>"
            "</forwarded>"
            "</%s>"
            "</message>",
            cc->carbon_dir);

  write_fn(write_ud, buf, len);
  stump_d("carbons: sent %s carbon to %s (%zu bytes)", cc->carbon_dir, full_jid, (size_t)len);
}

/* Send carbons <sent/> copies to all other carbons-enabled resources of the
 * sender, excluding the sending resource itself.
 * Also sends a <sent/> to the sending resource itself (the sending device's
 * own sent-copy, which RFC 6121 §8 does not deliver to the sender). */
static void send_carbon_sent(const char* sender_bare, const char* exclude_full_jid,
                              const char* orig_from, const char* orig_to,
                              const char* orig_type, const char* orig_id,
                              const char* orig_body) {
  carbon_copy_ctx_t cc = {
      .sender_bare = sender_bare,
      .orig_from = orig_from,
      .orig_to = orig_to,
      .orig_type = orig_type,
      .orig_id = orig_id,
      .orig_body = orig_body,
      .carbon_dir = "sent",
  };
  xmpp_session_table_for_each_carbon_resource(sender_bare, exclude_full_jid, send_carbon_copy, &cc);
  stump_d("carbons: sent dispatch for %s (from=%s)", sender_bare, orig_from);
}

/* Send carbons <received/> copies to all other carbons-enabled resources of the
 * recipient, excluding the receiving resource (which already received the
 * original stanza via RFC 6121 routing). */
static void send_carbon_received(const char* recipient_bare, const char* exclude_full_jid,
                                  const char* orig_from, const char* orig_to,
                                  const char* orig_type, const char* orig_id,
                                  const char* orig_body) {
  carbon_copy_ctx_t cc = {
      .sender_bare = recipient_bare,
      .orig_from = orig_from,
      .orig_to = orig_to,
      .orig_type = orig_type,
      .orig_id = orig_id,
      .orig_body = orig_body,
      .carbon_dir = "received",
  };
  xmpp_session_table_for_each_carbon_resource(recipient_bare, exclude_full_jid,
                                              send_carbon_copy, &cc);
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

  /* Check XEP-0280 §9: <private xmlns='urn:xmpp:carbons:2'/> → exclude from carbons. */
  int is_private = msg_has_private(stanza);

  /* Extract <body> text (allocated by libstrophe — must free). */
  xmpp_stanza_t* body_el = xmpp_stanza_get_child_by_name(stanza, "body");
  char* body_text = body_el ? xmpp_stanza_get_text(body_el) : NULL;

  char to_bare[JID_BUF_SIZE];
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
        /* RFC 6121 delivered to the target full JID resource.
         * Carbons: send <received/> copies to all OTHER carbons-enabled
         * resources of the recipient (to_bare). */
        if (!is_private) {
          send_carbon_received(to_bare, to_attr, ctx->bound_jid, to_attr,
                               msg_type, msg_id, body_text);
        }
        free(body_text);
        return;
      }
    }
    /* Fall through to bare-JID routing below. */
    fwd_len = 0;
  }

  /* Bare JID routing. */
  char sender_bare[JID_BUF_SIZE];
  msg_bare_jid(ctx->bound_jid, sender_bare, sizeof(sender_bare));

  if (strcmp(to_bare, sender_bare) == 0) {
    /* Self-addressed: deliver to all other resources of this user (RFC 6121 §8).
     * Carbons <sent/>: also send a <sent/> copy to ALL carbons-enabled
     * resources of the sender (including this one), so every device
     * receives the sent-carbon wrapper per XEP-0280 §5. */
    if (build_message(fwd, &fwd_len, sizeof(fwd), ctx->bound_jid, to_bare, msg_type, msg_id,
                      body_text) == 0) {
      xmpp_session_table_broadcast_except(sender_bare, ctx->bound_jid, fwd, fwd_len);
      xmpp_session_table_touch(ctx->bound_jid);
      if (!is_private) {
        send_carbon_sent(sender_bare, ctx->bound_jid, ctx->bound_jid, to_bare,
                         msg_type, msg_id, body_text);
      }
    }
  } else {
    /* Normal bare-JID routing: pick best resource. */
    char best_jid[JID_BUF_SIZE];
    if (xmpp_session_table_best_resource(to_bare, best_jid, sizeof(best_jid)) == 0) {
      if (build_message(fwd, &fwd_len, sizeof(fwd), ctx->bound_jid, best_jid, msg_type, msg_id,
                        body_text) == 0) {
        xmpp_session_table_write(best_jid, fwd, fwd_len);
        xmpp_session_table_touch(ctx->bound_jid);
        /* RFC 6121 delivered to best_jid.
         * Carbons: send <sent/> to all OTHER carbons-enabled resources of sender,
         * and <received/> to all OTHER carbons-enabled resources of recipient. */
        if (!is_private) {
          send_carbon_sent(sender_bare, ctx->bound_jid, ctx->bound_jid, to_attr,
                           msg_type, msg_id, body_text);
          send_carbon_received(to_bare, best_jid, ctx->bound_jid, to_attr,
                               msg_type, msg_id, body_text);
        }
      }
    } else {
      /* No online resource: store for offline delivery (XEP-0160).
       * Carbons: only send <sent/> carbon copies (no recipient is online,
       * so no <received/> copies apply). */
      stump_d("message: %s is offline, storing for later delivery", to_bare);
      if (build_message(fwd, &fwd_len, sizeof(fwd), ctx->bound_jid, to_bare, msg_type, msg_id,
                        body_text) == 0) {
        xep0160_store(ctx, to_bare, msg_id, fwd, fwd_len);
        xmpp_session_table_touch(ctx->bound_jid);
        if (!is_private) {
          send_carbon_sent(sender_bare, ctx->bound_jid, ctx->bound_jid, to_bare,
                           msg_type, msg_id, body_text);
        }
      }
    }
  }

  free(body_text);
}