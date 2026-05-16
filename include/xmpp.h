#ifndef XMPP_H
#define XMPP_H

/* Max full JID per RFC 7622 §3: localpart(1023) + '@' + domainpart(1023) + '/' + resourcepart(1023) + NUL. */
#define JID_BUF_SIZE 3073

#include <stddef.h>

/* XMPP stream negotiation states (RFC 6120). */
typedef enum {
  XMPP_STATE_DISCONNECTED,

  /* Transport */
  XMPP_STATE_CONNECTED_TCP,

  /* Initial stream (plaintext) */
  XMPP_STATE_STREAM_OPENED,
  XMPP_STATE_FEATURES_RECEIVED,

  /* TLS */
  XMPP_STATE_STARTTLS_SENT,
  XMPP_STATE_TLS_NEGOTIATED,

  /* Stream restart after TLS */
  XMPP_STATE_STREAM_RESTARTED_POST_TLS,
  XMPP_STATE_FEATURES_RECEIVED_POST_TLS,

  /* SASL */
  XMPP_STATE_SASL_NEGOTIATING,
  XMPP_STATE_SASL_SUCCESS,

  /* Stream restart after SASL */
  XMPP_STATE_STREAM_RESTARTED_POST_SASL,
  XMPP_STATE_FEATURES_RECEIVED_POST_SASL,

  /* Resource binding */
  XMPP_STATE_RESOURCE_BINDING,
  XMPP_STATE_BOUND,

  /* Ready */
  XMPP_STATE_ONLINE,

  XMPP_STATE_CLOSING,
  XMPP_STATE_CLOSED
} xmpp_state_t;

/* Callback type for synchronous writes. */
typedef int (*xmpp_write_fn)(void* ud, const char* data, size_t len);

/* XMPP stream error types (RFC 6120 §4.9). */
typedef enum {
  PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED,
  PPMXMPP_STREAM_ERROR_BAD_NAMESPACE,
  PPMXMPP_STREAM_ERROR_POLICY_VIOLATION,
  PPMXMPP_STREAM_ERROR_NOT_AUTHORIZED,
} ppmxmpp_stream_error_t;

/* Callback invoked when a stream-level error occurs. */
typedef void (*ppmxmpp_stream_error_fn)(ppmxmpp_stream_error_t error, void* ud);

#define XMPP_BUF_SIZE 8192

/* Per-connection XMPP session state. */
typedef struct {
  xmpp_state_t state;
  char conn_id[33];              /* connection ID for logging (hex, null-terminated) */
  char domain[1024];             /* extracted from to='' in <stream:stream> — RFC 7622 §3.2:
                                      max 1023 octets */
  char authcid[1024];            /* SASL authcid (local-part of JID) — RFC 7622 §3.3: max
                                      1023 octets */
  char resource[1024];           /* resourcepart — RFC 7622 §3.4: max 1023 octets + NUL */
  char bound_jid[JID_BUF_SIZE]; /* full JID per RFC 7622 §3 */
  char stream_id[17];            /* unique per-session stream ID — RFC 6120 §4.7.3 */
  char expected_stanza_ns[256];  /* expected namespace for out-of-order validation */
  char expected_stanza_name[64]; /* expected stanza name for out-of-order validation */
  int needs_parser_reset;        /* reset parser before next feed (after SASL success/failure)*/
  int needs_starttls_proceed;    /* sent <proceed/>, server must do TLS handshake */
  int failed_auth_count;         /* consecutive SASL failures on this connection */
  void* strophe_ctx;             /* opaque libstrophe xmpp_ctx_t * */
  void* parser;                  /* opaque parser_t * */
  xmpp_write_fn write_fn;        /* saved write callback */
  void* write_ud;                /* user data for write callback */
  int pending_error;             /* set by callbacks to signal fatal error */
  ppmxmpp_stream_error_fn stream_error_fn; /* stream error callback */
  void* stream_error_ud;                   /* user data for stream error callback */
  char out_buf[XMPP_BUF_SIZE];             /* outgoing response buffer */
  size_t out_len;
  char client_ns[256]; /* default namespace from xmlns='' in <stream:stream> */
  char stream_ns[256]; /* stream namespace from xmlns:stream='' in <stream:stream> */
  int carbons_enabled; /* XEP-0280: set by IQ enable before presence registration */
} xmpp_session_t;

/* Feed received bytes into the XMPP state machine.
 * Calls write_fn to send responses synchronously.
 * Returns 0 to keep the connection open, -1 to close it. */
int xmpp_feed(xmpp_session_t* ctx, const char* data, size_t len, xmpp_write_fn write_fn, void* ud);

/* Reset the XMPP session for a new connection. */
void xmpp_session_reset(xmpp_session_t* ctx);

/* Free libstrophe resources (parser + strophe context). */
void xmpp_session_cleanup(xmpp_session_t* ctx);

/* Register core IQ handlers (roster get/set).
 * Called during server initialization before handling connections.
 */
int xmpp_iq_register_handlers(void);

#endif /* XMPP_H */
