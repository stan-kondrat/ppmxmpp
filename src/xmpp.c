#include "xmpp.h"
#include "xmpp_sasl.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parser.h"
#include "strophe.h"
#include "stumpless.h"

typedef xmpp_ctx_t strophe_ctx_t;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Append a formatted string to the write buffer. */
static int write_append(xmpp_session_t* ctx, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(ctx->out_buf + ctx->out_len, sizeof(ctx->out_buf) - ctx->out_len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof(ctx->out_buf) - ctx->out_len) {
    return -1;
  }
  ctx->out_len += (size_t)n;
  return 0;
}

/* Send the buffered data and reset the buffer. */
static int write_flush(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* ud) {
  if (ctx->out_len == 0) {
    return 0;
  }
  int rc = write_fn(ud, ctx->out_buf, ctx->out_len);
  ctx->out_len = 0;
  return rc;
}

/* ------------------------------------------------------------------ */
/*  Stream header / footer                                             */
/* ------------------------------------------------------------------ */

static int send_stream_open(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* ud) {
  /* RFC 6120 §4.7.1: response MUST include from= set to server's FQDN.
   * RFC 6120 §4.7.3: response MUST include id= (unique, unpredictable).
   * RFC 6120 §4.7.2: to= MUST NOT be included unless client sent from=. */
  int rc = write_append(ctx,
                        "<?xml version='1.0'?>"
                        "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                        "xmlns='jabber:client' from='%s' id='%s' version='1.0' "
                        "xml:lang='en'>",
                        ctx->domain, ctx->stream_id);
  if (rc != 0) {
    return -1;
  }
  return write_flush(ctx, write_fn, ud);
}

/* ------------------------------------------------------------------ */
/*  Parser callbacks                                                   */
/* ------------------------------------------------------------------ */

static void on_stream_start(char* name, char** attrs, void* ud) {
  xmpp_session_t* ctx = (xmpp_session_t*)ud;
  (void)name;

  /* Read "to" attribute from attrs array: {name, value, name, value, ..., NULL}
   */
  const char* to_val = NULL;
  for (int i = 0; attrs[i] != NULL; i += 2) {
    if (strcmp(attrs[i], "to") == 0) {
      to_val = attrs[i + 1];
      break;
    }
  }

  if (to_val) {
    size_t to_len = strlen(to_val);
    if (to_len < sizeof(ctx->domain)) {
      memcpy(ctx->domain, to_val, to_len);
      ctx->domain[to_len] = '\0';
    }
  }
  if (!ctx->domain[0]) {
    strncpy(ctx->domain, "localhost", sizeof(ctx->domain) - 1);
  }

  if (send_stream_open(ctx, ctx->write_fn, ctx->write_ud) != 0) {
    ctx->pending_error = 1;
    return;
  }

  switch (ctx->state) {
  case XMPP_STATE_INIT:
    /* Send stream:features with SASL PLAIN. */
    if (write_append(ctx, "<stream:features>"
                          "<mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                          "<mechanism>PLAIN</mechanism>"
                          "</mechanisms>"
                          "</stream:features>") != 0) {
      ctx->pending_error = 1;
      return;
    }
    if (write_flush(ctx, ctx->write_fn, ctx->write_ud) != 0) {
      ctx->pending_error = 1;
      return;
    }
    ctx->state = XMPP_STATE_FEATURES;
    break;

  case XMPP_STATE_AUTHED:
    /* Send stream:features with bind. */
    if (write_append(ctx, "<stream:features>"
                          "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                          "<required/></bind>"
                          "</stream:features>") != 0) {
      ctx->pending_error = 1;
      return;
    }
    if (write_flush(ctx, ctx->write_fn, ctx->write_ud) != 0) {
      ctx->pending_error = 1;
      return;
    }
    ctx->state = XMPP_STATE_BIND;
    break;

  default:
    ctx->pending_error = 1;
    break;
  }
}

static void on_stream_end(char* name, void* ud) {
  (void)name;
  xmpp_session_t* ctx = (xmpp_session_t*)ud;
  ctx->state = XMPP_STATE_FAILED;
}

static void on_stanza(xmpp_stanza_t* stanza, void* ud) {
  xmpp_session_t* ctx = (xmpp_session_t*)ud;

  switch (ctx->state) {
  case XMPP_STATE_FEATURES: {
    /* Expect <auth mechanism='PLAIN'>...</auth> */
    const char* mech = xmpp_stanza_get_attribute(stanza, "mechanism");
    if (!mech || strcmp(mech, "PLAIN") != 0) {
      write_append(ctx, "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                        "<unsupported-mechanism/></failure>");
      write_flush(ctx, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_FAILED;
      ctx->pending_error = 1;
      return;
    }

    /* Get base64-encoded credentials text. */
    char* b64_text = xmpp_stanza_get_text(stanza);
    if (!b64_text) {
      ctx->pending_error = 1;
      return;
    }

    /* Authenticate (handle_sasl_plain decodes base64 internally). */
    int auth_rc = handle_sasl_plain(ctx, b64_text, ctx->write_fn, ctx->write_ud);
    free(b64_text);

    if (auth_rc != 0) {
      write_append(ctx, "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                        "<not-authorized/></failure>");
      write_flush(ctx, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_FAILED;
      ctx->pending_error = 1;
      return;
    }

    /* Send success. */
    write_append(ctx, "<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>");
    write_flush(ctx, ctx->write_fn, ctx->write_ud);
    ctx->needs_parser_reset = 1; /* parser must be reset before stream restart */
    ctx->state = XMPP_STATE_AUTHED;
    break;
  }

  case XMPP_STATE_BIND: {
    /* Expect <iq type='set' id='...' xmlns='jabber:client'>
     *   <bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>
     * </iq> */
    const char* iq_type = xmpp_stanza_get_attribute(stanza, "type");
    if (!iq_type || strcmp(iq_type, "set") != 0) {
      return;
    }

    const char* iq_id = xmpp_stanza_get_attribute(stanza, "id");
    xmpp_stanza_t* bind = xmpp_stanza_get_child_by_name(stanza, "bind");
    if (!bind) {
      return;
    }

    int rc;
    if (iq_id) {
      rc = write_append(ctx,
                        "<iq type='result' id='%s'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                        "<jid>%s@%s</jid>"
                        "</bind>"
                        "</iq>",
                        iq_id, ctx->authcid, ctx->domain);
    } else {
      rc = write_append(ctx,
                        "<iq type='result'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                        "<jid>%s@%s</jid>"
                        "</bind>"
                        "</iq>",
                        ctx->authcid, ctx->domain);
    }
    if (rc != 0) {
      ctx->pending_error = 1;
      return;
    }
    write_flush(ctx, ctx->write_fn, ctx->write_ud);
    ctx->state = XMPP_STATE_CONNECTED;
    break;
  }

  case XMPP_STATE_CONNECTED:
    /* No-op. Keep connection open. */
    break;

  default:
    break;
  }
}

/* ------------------------------------------------------------------ */
/*  Context management                                                 */
/* ------------------------------------------------------------------ */

/* Recreate the XML parser while preserving session state.
 * Safe to call outside of a parser callback (i.e. at the start of xmpp_feed).
 * Required before processing a stream restart after SASL success because
 * expat rejects a second <?xml?> declaration inside an open document. */
static void reset_parser(xmpp_session_t* ctx) {
  if (ctx->parser) {
    parser_free((parser_t*)ctx->parser);
    ctx->parser = NULL;
  }
  strophe_ctx_t* sc = (strophe_ctx_t*)ctx->strophe_ctx;
  ctx->parser = parser_new(sc, on_stream_start, on_stream_end, on_stanza, ctx);
}

void xmpp_session_cleanup(xmpp_session_t* ctx) {
  if (ctx->parser != NULL) {
    parser_free((parser_t*)ctx->parser);
    ctx->parser = NULL;
  }
  if (ctx->strophe_ctx != NULL) {
    xmpp_ctx_free((strophe_ctx_t*)ctx->strophe_ctx);
    ctx->strophe_ctx = NULL;
  }
}

void xmpp_session_reset(xmpp_session_t* ctx) {
  xmpp_session_cleanup(ctx);
  memset(ctx, 0, sizeof(*ctx));
  ctx->state = XMPP_STATE_INIT;

  /* RFC 6120 §4.7.3: stream ID MUST be unique and unpredictable. */
  {
    unsigned char rnd[8] = {0};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      (void)read(fd, rnd, sizeof(rnd));
      close(fd);
    }
    snprintf(ctx->stream_id, sizeof(ctx->stream_id), "%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0],
             rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
  }

  strophe_ctx_t* sc = xmpp_ctx_new(NULL, NULL);
  ctx->strophe_ctx = sc;
  ctx->parser = parser_new(sc, on_stream_start, on_stream_end, on_stanza, ctx);
}

/* ------------------------------------------------------------------ */
/*  Main feed function                                                 */
/* ------------------------------------------------------------------ */

int xmpp_feed(xmpp_session_t* ctx, const char* data, size_t len, xmpp_write_fn write_fn, void* ud) {
  ctx->write_fn = write_fn;
  ctx->write_ud = ud;
  ctx->pending_error = 0;

  if (ctx->needs_parser_reset) {
    reset_parser(ctx);
    ctx->needs_parser_reset = 0;
  }

  int rc = parser_feed((parser_t*)ctx->parser, (char*)data, (int)len);
  if (rc == 0) {
    return -1;
  }
  return ctx->pending_error ? -1 : 0;
}
