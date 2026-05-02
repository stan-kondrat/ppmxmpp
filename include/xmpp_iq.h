#ifndef XMPP_IQ_H
#define XMPP_IQ_H

#include "xmpp.h"

/* Opaque libstrophe stanza type — forward-declared to avoid including strophe.h here. */
typedef struct _xmpp_stanza_t xmpp_stanza_t;

/* Dispatch an IQ stanza received in XMPP_STATE_CONNECTED.
 * Writes the response via ctx->write_fn and flushes. */
void xmpp_iq_dispatch(xmpp_session_t* ctx, xmpp_stanza_t* stanza);

#endif /* XMPP_IQ_H */
