#ifndef XMPP_IQ_H
#define XMPP_IQ_H

#include "xmpp.h"
#include "xmpp_iq_dispatch.h"

/* Opaque libstrophe stanza type — forward-declared to avoid including strophe.h here. */
typedef struct _xmpp_stanza_t xmpp_stanza_t;

/* Dispatch an IQ stanza received in XMPP_STATE_CONNECTED.
 * This is now a thin wrapper around iq_dispatch() from xmpp_iq_dispatch.h.
 * Kept for API compatibility — use xmpp_iq_dispatch.h directly for new code.
 */
void xmpp_iq_dispatch(xmpp_session_t* ctx, xmpp_stanza_t* stanza);

#endif /* XMPP_IQ_H */