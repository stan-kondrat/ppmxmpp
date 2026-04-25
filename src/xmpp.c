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

/* Extract namespace declarations from raw XML data.
 * Sets *client_ns and *stream_ns to the values of xmlns='' and xmlns:stream=''. */
static void _extract_namespaces(const char* data, size_t len, char* client_ns, size_t client_ns_size, char* stream_ns, size_t stream_ns_size) {
  const char* p = data;
  const char* end = data + len;

  /* Find <stream:stream or <stream:stream */
  const char* tag = memmem(p, (size_t)(end - p), "<stream:stream", 14);
  if (!tag) tag = memmem(p, (size_t)(end - p), "<stream:", 8);
  if (!tag) return;

  /* Find the closing > of the start tag */
  const char* tag_end = memmem(tag, (size_t)(end - tag), ">", 1);
  if (!tag_end) return;

  /* Search for xmlns='' and xmlns:stream='' within the tag */
  const char* ns_start = tag;
  const char* ns_end = tag_end;

  /* Find xmlns='' (default namespace) */
  const char* p1 = memmem(ns_start, (size_t)(ns_end - ns_start), "xmlns='", 7);
  if (!p1) p1 = memmem(ns_start, (size_t)(ns_end - ns_start), "xmlns=\"", 7);
  if (p1) {
    p1 += 7;
    const char* q1 = memmem(p1, (size_t)(ns_end - p1), "'", 1);
    if (!q1) q1 = memmem(p1, (size_t)(ns_end - p1), "\"", 1);
    if (q1) {
      size_t ns_len = (size_t)(q1 - p1);
      if (ns_len < client_ns_size) {
        memcpy(client_ns, p1, ns_len);
        client_ns[ns_len] = '\0';
      }
    }
  }

  /* Find xmlns:stream='' */
  const char* p2 = memmem(ns_start, (size_t)(ns_end - ns_start), "xmlns:stream='", 14);
  if (!p2) p2 = memmem(ns_start, (size_t)(ns_end - ns_start), "xmlns:stream=\"", 14);
  if (p2) {
    p2 += 14;
    const char* q2 = memmem(p2, (size_t)(ns_end - p2), "'", 1);
    if (!q2) q2 = memmem(p2, (size_t)(ns_end - p2), "\"", 1);
    if (q2) {
      size_t ns_len = (size_t)(q2 - p2);
      if (ns_len < stream_ns_size) {
        memcpy(stream_ns, p2, ns_len);
        stream_ns[ns_len] = '\0';
      }
    }
  }
}

/* Append a formatted string to the write buffer. */
static int write_append(xmpp_session_t* ctx, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
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

/* Send a <stream:error> element and invoke the error callback. */
static void send_stream_error(xmpp_session_t* ctx, ppmxmpp_stream_error_t error,
                              xmpp_write_fn write_fn, void* ud) {
  const char* error_qname = NULL;

  switch (error) {
  case PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED:
    error_qname = "not-well-formed";
    break;
  case PPMXMPP_STREAM_ERROR_BAD_NAMESPACE:
    error_qname = "invalid-namespace";
    break;
  }

  write_append(ctx, "<stream:error><%s xmlns='urn:ietf:params:xml:ns:xmpp-streams'/></stream:error>",
               error_qname);
  write_flush(ctx, write_fn, ud);

  if (ctx->stream_error_fn) {
    ctx->stream_error_fn(error, ctx->stream_error_ud);
  }

  ctx->pending_error = 1;
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

/* Validate the <stream:stream> open tag per RFC 6120 §4.7.
 * Returns 0 on success, -1 if validation fails (error already sent). */
static int validate_stream_open(xmpp_session_t* ctx, char** attrs) {
  const char* xmlns = ctx->client_ns[0] ? ctx->client_ns : NULL;
  const char* xmlns_stream = ctx->stream_ns[0] ? ctx->stream_ns : NULL;
  const char* version = NULL;

  for (int i = 0; attrs[i] != NULL; i += 2) {
    if (strcmp(attrs[i], "version") == 0) {
      version = attrs[i + 1];
    }
  }

  if (!xmlns || strcmp(xmlns, "jabber:client") != 0) {
    send_stream_error(ctx, PPMXMPP_STREAM_ERROR_BAD_NAMESPACE, ctx->write_fn, ctx->write_ud);
    return -1;
  }

  if (!xmlns_stream || strcmp(xmlns_stream, "http://etherx.jabber.org/streams") != 0) {
    send_stream_error(ctx, PPMXMPP_STREAM_ERROR_BAD_NAMESPACE, ctx->write_fn, ctx->write_ud);
    return -1;
  }

  if (!version || strcmp(version, "1.0") != 0) {
    send_stream_error(ctx, PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED, ctx->write_fn, ctx->write_ud);
    return -1;
  }

  return 0;
}

static void on_stream_start(char* name, char** attrs, void* ud) {
  xmpp_session_t* ctx = (xmpp_session_t*)ud;

  /* The parser strips namespace via '\x1F' separator; name should be "stream". */
  if (!name || strcmp(name, "stream") != 0) {
    send_stream_error(ctx, PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED, ctx->write_fn, ctx->write_ud);
    return;
  }

  /* RFC 6120 §4.7: validate namespace and version before processing. */
  if (validate_stream_open(ctx, attrs) != 0) {
    return;
  }

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
  case XMPP_STATE_STARTTLS:
    /* Send stream:features with SASL PLAIN.
     * XMPP_STATE_INIT: Initial features before any auth.
     * XMPP_STATE_STARTTLS: Stream restart after TLS handshake. RFC 6120 §5. */
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
    /* Check for <starttls/> first (RFC 6120 §5). */
    if (strcmp(xmpp_stanza_get_name(stanza), "starttls") == 0) {
      const char* ns = xmpp_stanza_get_ns(stanza);
      if (!ns || strcmp(ns, "urn:ietf:params:xml:ns:xmpp-tls") != 0) {
        write_append(ctx, "<failure xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"
                          "<invalid-namespace/></failure>");
        write_flush(ctx, ctx->write_fn, ctx->write_ud);
        ctx->state = XMPP_STATE_FAILED;
        ctx->pending_error = 1;
        return;
      }
      /* Send <proceed/> to request TLS handshake. */
      write_append(ctx, "<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>");
      write_flush(ctx, ctx->write_fn, ctx->write_ud);
      ctx->needs_starttls_proceed = 1;
      ctx->state = XMPP_STATE_STARTTLS;
      break;
    }

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
    const xmpp_stanza_t* bind = xmpp_stanza_get_child_by_name(stanza, "bind");
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
  ctx->parser = parser_new(sc, &on_stream_start, &on_stream_end, &on_stanza, ctx);
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
  ctx->parser = parser_new(sc, &on_stream_start, &on_stream_end, &on_stanza, ctx);
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

  /* RFC 6120 §5: reset parser after TLS handshake completes. */
  if (ctx->needs_starttls_proceed) {
    reset_parser(ctx);
    ctx->needs_starttls_proceed = 0;
  }

  /* Strip XML declaration (<?xml ...?>) before feeding to parser.
   * libstrophe's PI handler rejects all PIs; the declaration is redundant
   * for parsing and forbidden on stream restart anyway (RFC 6120 §4.3.3). */
  const char* feed_data = data;
  size_t feed_len = len;
  if (len >= 5 && data[0] == '<' && data[1] == '?' && data[2] == 'x' && data[3] == 'm' && data[4] == 'l') {
    const char* decl_end = memmem(data, len, "?>", 2);
    if (decl_end) {
      feed_data = decl_end + 2;
      feed_len = len - (size_t)(feed_data - data);
    }
  }

  /* Extract namespace declarations from raw XML before feeding to parser.
   * libstrophe does not pass xmlns declarations as regular attributes. */
  _extract_namespaces(data, len, ctx->client_ns, sizeof(ctx->client_ns), ctx->stream_ns, sizeof(ctx->stream_ns));

  /* libstrophe's parser_feed takes char* but doesn't modify the data. */
  int rc = parser_feed((parser_t*)ctx->parser, (char*)feed_data, (int)feed_len);
  if (rc == 0) {
    send_stream_error(ctx, PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED, write_fn, ud);
    return -1;
  }

  return ctx->pending_error ? -1 : 0;
}
