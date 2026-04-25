#ifndef XMPP_H
#define XMPP_H

#include <stddef.h>

/* XMPP stream negotiation states. */
typedef enum {
  XMPP_STATE_INIT,          /* waiting for <stream:stream> */
  XMPP_STATE_FEATURES,      /* sent SASL/TLS features, waiting for <auth> or <starttls/> */
  XMPP_STATE_STARTTLS,      /* sent <proceed/>, waiting for TLS handshake to complete */
  XMPP_STATE_AUTHED,        /* sent <success/>, waiting for stream restart */
  XMPP_STATE_BIND,          /* sent bind feature, waiting for bind IQ */
  XMPP_STATE_CONNECTED,     /* sent bound JID, fully negotiated */
  XMPP_STATE_FAILED,        /* sent <failure/>, close connection */
} xmpp_state_t;

/* Callback type for synchronous writes. */
typedef int (*xmpp_write_fn)(void* ud, const char* data, size_t len);

/* XMPP stream error types (RFC 6120 §4.9). */
typedef enum {
  PPMXMPP_STREAM_ERROR_NOT_WELL_FORMED,
  PPMXMPP_STREAM_ERROR_BAD_NAMESPACE,
} ppmxmpp_stream_error_t;

/* Callback invoked when a stream-level error occurs. */
typedef void (*ppmxmpp_stream_error_fn)(ppmxmpp_stream_error_t error, void* ud);

#define XMPP_BUF_SIZE 8192

/* Per-connection XMPP session state. */
typedef struct {
  xmpp_state_t state;
  char domain[1024];           /* extracted from to='' in <stream:stream> — RFC 7622 §3.2:
                                   max 1023 octets */
  char authcid[1024];          /* SASL authcid (local-part of JID) — RFC 7622 §3.3: max
                                   1023 octets */
  char stream_id[17];          /* unique per-session stream ID — RFC 6120 §4.7.3 */
   int needs_parser_reset;      /* reset parser before next feed (after SASL success)
                                 */
  int needs_starttls_proceed;  /* sent <proceed/>, server must do TLS handshake */
  void* strophe_ctx;           /* opaque libstrophe xmpp_ctx_t * */
  void* parser;                /* opaque parser_t * */
  xmpp_write_fn write_fn;      /* saved write callback */
  void* write_ud;              /* user data for write callback */
  int pending_error;           /* set by callbacks to signal fatal error */
  ppmxmpp_stream_error_fn stream_error_fn; /* stream error callback */
   void* stream_error_ud;      /* user data for stream error callback */
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
