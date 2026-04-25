#ifndef XMPP_H
#define XMPP_H

#include <stddef.h>

/* XMPP stream negotiation states. */
typedef enum {
    XMPP_STATE_INIT,         /* waiting for <stream:stream> */
    XMPP_STATE_FEATURES,     /* sent SASL features, waiting for <auth> */
    XMPP_STATE_AUTHED,       /* sent <success/>, waiting for stream restart */
    XMPP_STATE_BIND,         /* sent bind feature, waiting for bind IQ */
    XMPP_STATE_CONNECTED,    /* sent bound JID, fully negotiated */
    XMPP_STATE_FAILED,       /* sent <failure/>, close connection */
} xmpp_state_t;

/* Callback type for synchronous writes. */
typedef int (*xmpp_write_fn)(void *ud, const char *data, size_t len);

#define XMPP_BUF_SIZE 8192

/* Per-connection XMPP session state. */
typedef struct {
    xmpp_state_t state;
    char         domain[1024];     /* extracted from to='' in <stream:stream> — RFC 7622 §3.2: max 1023 octets */
    char         authcid[1024];    /* SASL authcid (local-part of JID) — RFC 7622 §3.3: max 1023 octets */
    char         stream_id[17];    /* unique per-session stream ID — RFC 6120 §4.7.3 */
    int          needs_parser_reset; /* reset parser before next feed (after SASL success) */
    void        *strophe_ctx;      /* opaque libstrophe xmpp_ctx_t * */
    void        *parser;           /* opaque parser_t * */
    xmpp_write_fn write_fn;        /* saved write callback */
    void        *write_ud;         /* user data for write callback */
    int          pending_error;    /* set by callbacks to signal fatal error */
    char         out_buf[XMPP_BUF_SIZE]; /* outgoing response buffer */
    size_t       out_len;
} xmpp_session_t;

/* Feed received bytes into the XMPP state machine.
 * Calls write_fn to send responses synchronously.
 * Returns 0 to keep the connection open, -1 to close it. */
int xmpp_feed(xmpp_session_t *ctx, const char *data, size_t len,
               xmpp_write_fn write_fn, void *ud);

/* Reset the XMPP session for a new connection. */
void xmpp_session_reset(xmpp_session_t *ctx);

/* Free libstrophe resources (parser + strophe context). */
void xmpp_session_cleanup(xmpp_session_t *ctx);

#endif /* XMPP_H */
