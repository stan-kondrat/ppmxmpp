#include "xmpp.h"
#include "xmpp_iq.h"
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
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static int _send_stream_features(xmpp_session_t* ctx);
static int _send_stream_close(xmpp_session_t* ctx);
static const char* _state_name(xmpp_state_t state);

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Extract namespace declarations from raw XML data.
 * Sets *client_ns and *stream_ns to the values of xmlns='' and xmlns:stream=''. */
static void _extract_namespaces(const char* data, size_t len, char* client_ns,
                                size_t client_ns_size, char* stream_ns, size_t stream_ns_size) {
  const char* p = data;
  const char* end = data + len;

  /* Find <stream:stream or <stream:stream */
  const char* tag = memmem(p, (size_t)(end - p), "<stream:stream", 14);
  if (!tag) {
    tag = memmem(p, (size_t)(end - p), "<stream:", 8);
  }
  if (!tag) {
    return;
  }

  /* Find the closing > of the start tag */
  const char* tag_end = memmem(tag, (size_t)(end - tag), ">", 1);
  if (!tag_end) {
    return;
  }

  /* Search for xmlns='' and xmlns:stream='' within the tag */
  const char* ns_start = tag;
  const char* ns_end = tag_end;

  /* Find xmlns='' (default namespace) */
  const char* p1 = memmem(ns_start, (size_t)(ns_end - ns_start), "xmlns='", 7);
  if (!p1) {
    p1 = memmem(ns_start, (size_t)(ns_end - ns_start), "xmlns=\"", 7);
  }
  if (p1) {
    p1 += 7;
    const char* q1 = memmem(p1, (size_t)(ns_end - p1), "'", 1);
    if (!q1) {
      q1 = memmem(p1, (size_t)(ns_end - p1), "\"", 1);
    }
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
  if (!p2) {
    p2 = memmem(ns_start, (size_t)(ns_end - ns_start), "xmlns:stream=\"", 14);
  }
  if (p2) {
    p2 += 14;
    const char* q2 = memmem(p2, (size_t)(ns_end - p2), "'", 1);
    if (!q2) {
      q2 = memmem(p2, (size_t)(ns_end - p2), "\"", 1);
    }
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
static int write_append(xmpp_session_t* ctx, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
static int write_append(xmpp_session_t* ctx, const char* fmt, ...) {
  va_list ap;
  va_list ap_save;
  va_start(ap, fmt);
  va_copy(ap_save, ap);
  int n = vsnprintf(ctx->out_buf + ctx->out_len, sizeof(ctx->out_buf) - ctx->out_len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof(ctx->out_buf) - ctx->out_len) {
    /* Buffer full — flush what we have so far, then retry. */
    va_end(ap_save);
    return -1;
  }
  ctx->out_len += (size_t)n;
  va_end(ap_save);
  return 0;
}

static int _is_forbidden_resource_char(unsigned char ch) {
  if (ch < 0x20 || ch == 0x7f) {
    return 1;
  }
  switch (ch) {
  case '"':
  case '\'':
  case '/':
  case ':':
  case '<':
  case '>':
  case '@':
    return 1;
  }
  return 0;
}

/* Returns 0 if resource is valid per RFC 7622 §3.4 (stub), -1 otherwise. */
static int _validate_resource(const char* res) {
  size_t len = strlen(res);
  if (len == 0 || len > 1023) {
    return -1;
  }
  for (size_t i = 0; i < len; i++) {
    if (_is_forbidden_resource_char((unsigned char)res[i])) {
      return -1;
    }
  }
  return 0;
}

/* Write 8 random hex characters into buf (must be >= 9 bytes). */
static void _gen_resource(char* buf) {
  unsigned char rnd[4] = {0};
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    (void)read(fd, rnd, sizeof(rnd));
    close(fd);
  }
  snprintf(buf, 9, "%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3]);
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
  case PPMXMPP_STREAM_ERROR_POLICY_VIOLATION:
    error_qname = "policy-violation";
    break;
  case PPMXMPP_STREAM_ERROR_NOT_AUTHORIZED:
    error_qname = "not-authorized";
    break;
  }

  stump_d("stream error conn_id='%s' state=%s error=%s", ctx->conn_id, _state_name(ctx->state),
          error_qname);

  write_append(ctx,
               "<stream:error><%s xmlns='urn:ietf:params:xml:ns:xmpp-streams'/></stream:error>",
               error_qname);
  write_flush(ctx, write_fn, ud);

  /* Send </stream:stream> after stream:error per RFC 6120 §4.4.4. */
  _send_stream_close(ctx);

  if (ctx->stream_error_fn) {
    ctx->stream_error_fn(error, ctx->stream_error_ud);
  }

  ctx->pending_error = 1;
}

/* ------------------------------------------------------------------ */
/*  State transition helpers                                           */
/* ------------------------------------------------------------------ */

typedef enum {
  XMPP_EVENT_STREAM_OPEN,
  XMPP_EVENT_AUTH,
  XMPP_EVENT_SASL_SUCCESS,
  XMPP_EVENT_CLOSE,
  XMPP_EVENT_TCP_CLOSE,
} xmpp_event_t;

static const char* _event_name(xmpp_event_t event) {
  switch (event) {
  case XMPP_EVENT_STREAM_OPEN:
    return "stream_open";
  case XMPP_EVENT_AUTH:
    return "auth";
  case XMPP_EVENT_SASL_SUCCESS:
    return "sasl_success";
  case XMPP_EVENT_CLOSE:
    return "close";
  case XMPP_EVENT_TCP_CLOSE:
    return "tcp_close";
  }
  return "unknown";
}

static const char* _state_name(xmpp_state_t state) {
  switch (state) {
  case XMPP_STATE_DISCONNECTED:
    return "DISCONNECTED";
  case XMPP_STATE_CONNECTED_TCP:
    return "CONNECTED_TCP";
  case XMPP_STATE_STREAM_OPENED:
    return "STREAM_OPENED";
  case XMPP_STATE_FEATURES_RECEIVED:
    return "FEATURES_RECEIVED";
  case XMPP_STATE_STARTTLS_SENT:
    return "STARTTLS_SENT";
  case XMPP_STATE_TLS_NEGOTIATED:
    return "TLS_NEGOTIATED";
  case XMPP_STATE_STREAM_RESTARTED_POST_TLS:
    return "STREAM_RESTARTED_POST_TLS";
  case XMPP_STATE_FEATURES_RECEIVED_POST_TLS:
    return "FEATURES_RECEIVED_POST_TLS";
  case XMPP_STATE_SASL_NEGOTIATING:
    return "SASL_NEGOTIATING";
  case XMPP_STATE_SASL_SUCCESS:
    return "SASL_SUCCESS";
  case XMPP_STATE_STREAM_RESTARTED_POST_SASL:
    return "STREAM_RESTARTED_POST_SASL";
  case XMPP_STATE_FEATURES_RECEIVED_POST_SASL:
    return "FEATURES_RECEIVED_POST_SASL";
  case XMPP_STATE_RESOURCE_BINDING:
    return "RESOURCE_BINDING";
  case XMPP_STATE_BOUND:
    return "BOUND";
  case XMPP_STATE_ONLINE:
    return "ONLINE";
  case XMPP_STATE_CLOSING:
    return "CLOSING";
  case XMPP_STATE_CLOSED:
    return "CLOSED";
  }
  return "UNKNOWN";
}

/* Transition validation table: (from_state, event) → to_state.
 * Returns the target state, or XMPP_STATE_CLOSED for invalid transitions. */
static xmpp_state_t _validate_transition(xmpp_state_t from, xmpp_event_t event) {
  switch (event) {
  case XMPP_EVENT_STREAM_OPEN:
    if (from == XMPP_STATE_CONNECTED_TCP) {
      return XMPP_STATE_STREAM_OPENED;
    }
    if (from == XMPP_STATE_TLS_NEGOTIATED || from == XMPP_STATE_STARTTLS_SENT) {
      return XMPP_STATE_STREAM_RESTARTED_POST_TLS;
    }
    if (from == XMPP_STATE_SASL_SUCCESS) {
      return XMPP_STATE_STREAM_RESTARTED_POST_SASL;
    }
    break;
  case XMPP_EVENT_AUTH:
    if (from == XMPP_STATE_FEATURES_RECEIVED_POST_TLS) {
      return XMPP_STATE_SASL_NEGOTIATING;
    }
    break;
  case XMPP_EVENT_SASL_SUCCESS:
    if (from == XMPP_STATE_SASL_NEGOTIATING) {
      return XMPP_STATE_SASL_SUCCESS;
    }
    break;
  case XMPP_EVENT_CLOSE:
    if (from == XMPP_STATE_ONLINE || from == XMPP_STATE_CLOSING) {
      return XMPP_STATE_CLOSING;
    }
    break;
  case XMPP_EVENT_TCP_CLOSE:
    if (from == XMPP_STATE_CLOSING) {
      return XMPP_STATE_CLOSED;
    }
    break;
  }
  return XMPP_STATE_CLOSED;
}

/* Transition to a new state, send stream:features, then advance to the
 * stable FEATURES_RECEIVED_* / BOUND state.  Returns 0 on success, -1 on error. */
static int _transition_to(xmpp_session_t* ctx, xmpp_state_t new_state, xmpp_event_t event) {
  xmpp_state_t from_state = ctx->state;

  xmpp_state_t allowed = _validate_transition(from_state, event);
  if (allowed == XMPP_STATE_CLOSED && new_state != XMPP_STATE_CLOSED) {
    stump_w("stream invalid-transition conn_id='%s' from=%s event=%s", ctx->conn_id,
            _state_name(from_state), _event_name(event));
    send_stream_error(ctx, PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED, ctx->write_fn, ctx->write_ud);
    ctx->state = XMPP_STATE_CLOSING;
    ctx->pending_error = 1;
    return -1;
  }

  ctx->state = new_state;

  if (_send_stream_features(ctx) != 0) {
    ctx->pending_error = 1;
    return -1;
  }
  if (write_flush(ctx, ctx->write_fn, ctx->write_ud) != 0) {
    ctx->pending_error = 1;
    return -1;
  }

  /* Advance past the transient STREAM_RESTARTED_* states to the stable
   * FEATURES_RECEIVED_* / BOUND states where stanza handlers wait. */
  if (new_state == XMPP_STATE_STREAM_OPENED) {
    ctx->state = XMPP_STATE_FEATURES_RECEIVED;
  } else if (new_state == XMPP_STATE_STREAM_RESTARTED_POST_TLS) {
    ctx->state = XMPP_STATE_FEATURES_RECEIVED_POST_TLS;
  } else if (new_state == XMPP_STATE_STREAM_RESTARTED_POST_SASL) {
    ctx->state = XMPP_STATE_BOUND;
  }

  stump_d("stream transition conn_id='%s' from=%s event=%s to=%s", ctx->conn_id,
          _state_name(from_state), _event_name(event), _state_name(ctx->state));

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Features advertisement                                             */
/* ------------------------------------------------------------------ */

static int _send_stream_features(xmpp_session_t* ctx) {
  switch (ctx->state) {
  case XMPP_STATE_STREAM_OPENED:
    /* RFC 6120 §5: offer STARTTLS before any auth. */
    if (write_append(ctx, "<stream:features>"
                          "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"
                          "<required/>"
                          "</starttls>"
                          "</stream:features>") != 0) {
      return -1;
    }
    break;

  case XMPP_STATE_STREAM_RESTARTED_POST_TLS:
    /* TLS active — offer SASL mechanisms. */
    if (write_append(ctx, "<stream:features>"
                          "<mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                          "<mechanism>PLAIN</mechanism>"
                          "</mechanisms>"
                          "</stream:features>") != 0) {
      return -1;
    }
    break;

  case XMPP_STATE_STREAM_RESTARTED_POST_SASL:
    /* RFC 6120 §7: offer resource bind after SASL success and stream restart. */
    if (write_append(ctx, "<stream:features>"
                          "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                          "<required/></bind>"
                          "</stream:features>") != 0) {
      return -1;
    }
    break;

  default:
    break;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Stream header / footer                                             */
/* ------------------------------------------------------------------ */

/* Send </stream:stream> end element and transition to CLOSING.
 * Per RFC 6120 §4.4.4, the server MAY close the stream at any time. */
static int _send_stream_close(xmpp_session_t* ctx) {
  if (ctx->state == XMPP_STATE_CLOSING || ctx->state == XMPP_STATE_CLOSED) {
    return 0;
  }

  stump_d("stream close conn_id='%s' state=%s", ctx->conn_id, _state_name(ctx->state));

  int rc = write_append(ctx, "</stream:stream>");
  if (rc != 0) {
    return -1;
  }
  rc = write_flush(ctx, ctx->write_fn, ctx->write_ud);
  if (rc != 0) {
    return -1;
  }

  ctx->state = XMPP_STATE_CLOSING;
  return 0;
}

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

  stump_d("stream open conn_id='%s' domain=%s", ctx->conn_id, ctx->domain);

  if (send_stream_open(ctx, ctx->write_fn, ctx->write_ud) != 0) {
    ctx->pending_error = 1;
    return;
  }

  /* Transition based on current state. */
  int rc = -1;
  switch (ctx->state) {
  case XMPP_STATE_CONNECTED_TCP:
    rc = _transition_to(ctx, XMPP_STATE_STREAM_OPENED, XMPP_EVENT_STREAM_OPEN);
    break;
  case XMPP_STATE_TLS_NEGOTIATED:
  case XMPP_STATE_STARTTLS_SENT:
    rc = _transition_to(ctx, XMPP_STATE_STREAM_RESTARTED_POST_TLS, XMPP_EVENT_STREAM_OPEN);
    break;
  case XMPP_STATE_SASL_SUCCESS:
    rc = _transition_to(ctx, XMPP_STATE_STREAM_RESTARTED_POST_SASL, XMPP_EVENT_STREAM_OPEN);
    break;
  default:
    stump_w("stream unexpected-open conn_id='%s' state=%s", ctx->conn_id, _state_name(ctx->state));
    ctx->pending_error = 1;
    break;
  }
  if (rc != 0) {
    return;
  }
}

static void on_stream_end(char* name, void* ud) {
  (void)name;
  xmpp_session_t* ctx = (xmpp_session_t*)ud;

  stump_d("stream end conn_id='%s' state=%s", ctx->conn_id, _state_name(ctx->state));

  /* RFC 6120 §4.4.4: on receiving </stream:stream>, send </stream:stream> in response. */
  if (ctx->state != XMPP_STATE_CLOSING && ctx->state != XMPP_STATE_CLOSED) {
    _send_stream_close(ctx);
    ctx->state = XMPP_STATE_CLOSING;
  }
}

static void on_stanza(xmpp_stanza_t* stanza, void* ud) {
  xmpp_session_t* ctx = (xmpp_session_t*)ud;

  stump_d("stream stanza conn_id='%s' state=%s stanza=%s", ctx->conn_id, _state_name(ctx->state),
          xmpp_stanza_get_name(stanza));

  switch (ctx->state) {
  case XMPP_STATE_FEATURES_RECEIVED: {
    /* RFC 6120 §5: only <starttls/> expected before TLS is active.
     * <auth> before TLS = unsupported mechanism (RFC 6120 §6.3.6). */
    if (strcmp(xmpp_stanza_get_name(stanza), "auth") == 0) {
      const char* mech_pre = xmpp_stanza_get_attribute(stanza, "mechanism");
      if (mech_pre && strcmp(mech_pre, "PLAIN") == 0) {
        /* RFC 6120 §6.3.6 + §5.1: sending PLAIN credentials before TLS is a
         * policy-violation stream error — close with CLOSING (client must ack). */
        stump_d("stream sasl-plain-before-tls conn_id='%s'", ctx->conn_id);
        send_stream_error(ctx, PPMXMPP_STREAM_ERROR_POLICY_VIOLATION, ctx->write_fn, ctx->write_ud);
        ctx->state = XMPP_STATE_CLOSING;
        ctx->pending_error = 1;
      } else {
        /* Other mechanisms before TLS: SASL <failure> + </stream:stream> → CLOSED. */
        stump_d("stream sasl-before-tls conn_id='%s'", ctx->conn_id);
        write_append(ctx, "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                          "<unsupported-mechanism/></failure>");
        write_append(ctx, "</stream:stream>");
        write_flush(ctx, ctx->write_fn, ctx->write_ud);
        ctx->state = XMPP_STATE_CLOSED;
        ctx->pending_error = 1;
      }
      break;
    }
    if (strcmp(xmpp_stanza_get_name(stanza), "starttls") != 0) {
      stump_d("stream out-of-order conn_id='%s' state=%s stanza=%s", ctx->conn_id,
              _state_name(ctx->state), xmpp_stanza_get_name(stanza));
      send_stream_error(ctx, PPMXMPP_STREAM_ERROR_POLICY_VIOLATION, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_CLOSING;
      ctx->pending_error = 1;
      break;
    }
    const char* tls_ns = xmpp_stanza_get_ns(stanza);
    if (!tls_ns || strcmp(tls_ns, "urn:ietf:params:xml:ns:xmpp-tls") != 0) {
      /* RFC 6120 §5.4.2.2: TLS failure — send <failure> + </stream:stream> and go to
       * CLOSING (client must acknowledge the closing stream tag). */
      stump_d("stream starttls-invalid-ns conn_id='%s'", ctx->conn_id);
      write_append(ctx, "<failure xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"
                        "<invalid-namespace/></failure>");
      write_append(ctx, "</stream:stream>");
      write_flush(ctx, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_CLOSING;
      ctx->pending_error = 1;
      break;
    }
    stump_d("stream starttls conn_id='%s'", ctx->conn_id);
    write_append(ctx, "<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>");
    write_flush(ctx, ctx->write_fn, ctx->write_ud);
    ctx->needs_starttls_proceed = 1;
    ctx->state = XMPP_STATE_STARTTLS_SENT;
    break;
  }

  case XMPP_STATE_FEATURES_RECEIVED_POST_TLS: {
    /* TLS active — only <auth/> expected. */
    if (strcmp(xmpp_stanza_get_name(stanza), "auth") != 0) {
      stump_d("stream out-of-order conn_id='%s' state=%s stanza=%s", ctx->conn_id,
              _state_name(ctx->state), xmpp_stanza_get_name(stanza));
      send_stream_error(ctx, PPMXMPP_STREAM_ERROR_POLICY_VIOLATION, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_CLOSING;
      ctx->pending_error = 1;
      break;
    }

    const char* mech = xmpp_stanza_get_attribute(stanza, "mechanism");
    if (!mech || strcmp(mech, "PLAIN") != 0) {
      /* RFC 6120 §6.3.6: SASL failure is terminal — send <failure> + </stream:stream>. */
      stump_d("stream sasl-unsupported-mech conn_id='%s' mech=%s", ctx->conn_id,
              mech ? mech : "(none)");
      write_append(ctx, "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                        "<unsupported-mechanism/></failure>");
      write_append(ctx, "</stream:stream>");
      write_flush(ctx, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_CLOSED;
      ctx->pending_error = 1;
      break;
    }

    stump_d("stream sasl-auth conn_id='%s' mech=%s", ctx->conn_id, mech);

    char* b64_text = xmpp_stanza_get_text(stanza);
    if (!b64_text) {
      stump_w("stream sasl-empty-credentials conn_id='%s'", ctx->conn_id);
      ctx->pending_error = 1;
      break;
    }

    int auth_rc = handle_sasl_plain(ctx, b64_text, ctx->write_fn, ctx->write_ud);
    free(b64_text);

    if (auth_rc != 0) {
      write_append(ctx, "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                        "<not-authorized/></failure>");
      write_flush(ctx, ctx->write_fn, ctx->write_ud);
      if (auth_rc == -2) {
        /* Terminal protocol violation — close immediately. */
        send_stream_error(ctx, PPMXMPP_STREAM_ERROR_POLICY_VIOLATION, ctx->write_fn, ctx->write_ud);
        ctx->state = XMPP_STATE_CLOSING;
      } else {
        ctx->failed_auth_count++;
        stump_d("stream sasl-failure conn_id='%s' attempt=%d", ctx->conn_id,
                ctx->failed_auth_count);
        if (ctx->failed_auth_count >= 3) {
          send_stream_error(ctx, PPMXMPP_STREAM_ERROR_POLICY_VIOLATION, ctx->write_fn,
                            ctx->write_ud);
          ctx->state = XMPP_STATE_CLOSING;
        } else {
          /* Allow client to reopen the stream and retry. */
          ctx->needs_parser_reset = 1;
          ctx->state = XMPP_STATE_TLS_NEGOTIATED;
        }
      }
      break;
    }

    stump_d("stream sasl-success conn_id='%s'", ctx->conn_id);
    write_append(ctx, "<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>");
    write_flush(ctx, ctx->write_fn, ctx->write_ud);
    ctx->needs_parser_reset = 1;
    ctx->state = XMPP_STATE_SASL_SUCCESS;
    break;
  }

  case XMPP_STATE_BOUND: {
    /* RFC 6120 §7: expect bind IQ after stream restart post-SASL. */
    const char* iq_type = xmpp_stanza_get_attribute(stanza, "type");
    if (!iq_type || strcmp(iq_type, "set") != 0) {
      stump_d("stream out-of-order conn_id='%s' state=%s stanza=%s", ctx->conn_id,
              _state_name(ctx->state), xmpp_stanza_get_name(stanza));
      send_stream_error(ctx, PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_CLOSING;
      ctx->pending_error = 1;
      return;
    }

    const char* iq_id = xmpp_stanza_get_attribute(stanza, "id");
    const xmpp_stanza_t* bind = xmpp_stanza_get_child_by_name(stanza, "bind");
    if (!bind) {
      stump_d("stream out-of-order conn_id='%s' state=%s stanza=%s", ctx->conn_id,
              _state_name(ctx->state), xmpp_stanza_get_name(stanza));
      send_stream_error(ctx, PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED, ctx->write_fn, ctx->write_ud);
      ctx->state = XMPP_STATE_CLOSING;
      ctx->pending_error = 1;
      return;
    }

    xmpp_stanza_t* res_el = xmpp_stanza_get_child_by_name((xmpp_stanza_t*)bind, "resource");
    /* xmpp_stanza_get_text allocates — must free; get_text_ptr only works on text nodes. */
    char* res_text = res_el ? xmpp_stanza_get_text(res_el) : NULL;

    if (res_text && res_text[0] != '\0' && _validate_resource(res_text) != 0) {
      stump_d("stream bind-invalid-resource conn_id='%s'", ctx->conn_id);
      free(res_text);
      int bad_rc;
      if (iq_id) {
        bad_rc = write_append(ctx,
                              "<iq type='error' id='%s'>"
                              "<error type='modify'>"
                              "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                              "</error>"
                              "</iq>",
                              iq_id);
      } else {
        bad_rc = write_append(ctx, "<iq type='error'>"
                                   "<error type='modify'>"
                                   "<bad-request xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                                   "</error>"
                                   "</iq>");
      }
      if (bad_rc != 0) {
        ctx->pending_error = 1;
        return;
      }
      write_flush(ctx, ctx->write_fn, ctx->write_ud);
      return; /* stay in BOUND — client may retry with valid resource */
    }

    char res_buf[9];
    const char* resource;
    if (res_text && res_text[0] != '\0') {
      resource = res_text;
    } else {
      free(res_text);
      res_text = NULL;
      _gen_resource(res_buf);
      resource = res_buf;
    }

    snprintf(ctx->resource, sizeof(ctx->resource), "%s", resource);
    free(res_text);
    snprintf(ctx->bound_jid, sizeof(ctx->bound_jid), "%s@%s/%s", ctx->authcid, ctx->domain,
             ctx->resource);

    stump_d("stream bind conn_id='%s' iq_id=%s resource=%s", ctx->conn_id, iq_id ? iq_id : "(none)",
            ctx->resource);

    int rc;
    if (iq_id) {
      rc = write_append(ctx,
                        "<iq type='result' id='%s'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                        "<jid>%s</jid>"
                        "</bind>"
                        "</iq>",
                        iq_id, ctx->bound_jid);
    } else {
      rc = write_append(ctx,
                        "<iq type='result'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                        "<jid>%s</jid>"
                        "</bind>"
                        "</iq>",
                        ctx->bound_jid);
    }
    if (rc != 0) {
      ctx->pending_error = 1;
      return;
    }
    write_flush(ctx, ctx->write_fn, ctx->write_ud);
    stump_d("stream bind-success conn_id='%s' jid=%s", ctx->conn_id, ctx->bound_jid);
    ctx->state = XMPP_STATE_ONLINE;
    break;
  }

  case XMPP_STATE_ONLINE:
    /* Post-bind session: dispatch IQ stanzas via the IQ router. */
    if (strcmp(xmpp_stanza_get_name(stanza), "iq") == 0) {
      xmpp_iq_dispatch(ctx, stanza);
    }
    break;

  default:
    stump_w("stream unexpected-stanza conn_id='%s' state=%s stanza=%s", ctx->conn_id,
            _state_name(ctx->state), xmpp_stanza_get_name(stanza));
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
  ctx->state = XMPP_STATE_CONNECTED_TCP;

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
  if (len >= 5 && data[0] == '<' && data[1] == '?' && data[2] == 'x' && data[3] == 'm' &&
      data[4] == 'l') {
    const char* decl_end = memmem(data, len, "?>", 2);
    if (decl_end) {
      feed_data = decl_end + 2;
      feed_len = len - (size_t)(feed_data - data);
    }
  }

  /* Extract namespace declarations from raw XML before feeding to parser.
   * libstrophe does not pass xmlns declarations as regular attributes. */
  _extract_namespaces(data, len, ctx->client_ns, sizeof(ctx->client_ns), ctx->stream_ns,
                      sizeof(ctx->stream_ns));

  /* libstrophe's parser_feed takes char* but doesn't modify the data. */
  int rc = parser_feed((parser_t*)ctx->parser, (char*)feed_data, (int)feed_len);
  if (rc == 0) {
    send_stream_error(ctx, PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED, write_fn, ud);
    return -1;
  }

  return ctx->pending_error ? -1 : 0;
}
