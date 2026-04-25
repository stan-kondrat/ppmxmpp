#ifndef XMPP_H
#define XMPP_H

#include <stddef.h>

/* XMPP stream negotiation states (RFC 6120). */
typedef enum {
  XMPP_STATE_CONNECTED,
  XMPP_STATE_STREAM_OPENED_PLAINTEXT,
  XMPP_STATE_TLS_HANDSHAKING,
  XMPP_STATE_STREAM_OPENED_TLS,
  XMPP_STATE_SASL_AUTHENTICATING,
  XMPP_STATE_STREAM_OPENED_AUTHENTICATED,
  XMPP_STATE_RESOURCE_BOUND,
  XMPP_STATE_CLOSING,
  XMPP_STATE_CLOSED,
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
  char conn_id[33];            /* connection ID for logging (hex, null-terminated) */
  char domain[1024];           /* extracted from to='' in <stream:stream> — RFC 7622 §3.2:
                                    max 1023 octets */
  char authcid[1024];          /* SASL authcid (local-part of JID) — RFC 7622 §3.3: max
                                    1023 octets */
  char stream_id[17];          /* unique per-session stream ID — RFC 6120 §4.7.3 */
  char expected_stanza_ns[256];/* expected namespace for out-of-order validation */
  char expected_stanza_name[64];/* expected stanza name for out-of-order validation */
  int needs_parser_reset;      /* reset parser before next feed (after SASL success)
                               */
  int needs_starttls_proceed;  /* sent <proceed/>, server must do TLS handshake */
  void* strophe_ctx;           /* opaque libstrophe xmpp_ctx_t * */
  void* parser;                /* opaque parser_t * */
  xmpp_write_fn write_fn;      /* saved write callback */
  void* write_ud;              /* user data for write callback */
  int pending_error;           /* set by callbacks to signal fatal error */
  ppmxmpp_stream_error_fn stream_error_fn; /* stream error callback */
  void* stream_error_ud;       /* user data for stream error callback */
  char out_buf[XMPP_BUF_SIZE]; /* outgoing response buffer */
  size_t out_len;
  char client_ns[256];         /* default namespace from xmlns='' in <stream:stream> */
  char stream_ns[256];         /* stream namespace from xmlns:stream='' in <stream:stream> */
} xmpp_session_t;

/* Feed received bytes into the XMPP state machine.
 * Calls write_fn to send responses synchronously.
 * Returns 0 to keep the connection open, -1 to close it. */
int xmpp_feed(xmpp_session_t* ctx, const char* data, size_t len, xmpp_write_fn write_fn, void* ud);

/* Reset the XMPP session for a new connection. */
void xmpp_session_reset(xmpp_session_t* ctx);

/* Free libstrophe resources (parser + strophe context). */
void xmpp_session_cleanup(xmpp_session_t* ctx);

#endif /* XMPP_H */
