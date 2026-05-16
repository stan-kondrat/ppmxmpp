#ifndef XMPP_PRESENCE_H
#define XMPP_PRESENCE_H

#include "xmpp.h"
#include "xmpp_session.h"

/* Opaque libstrophe stanza type — forward-declared to avoid including strophe.h here. */
typedef struct _xmpp_stanza_t xmpp_stanza_t;

/* ------------------------------------------------------------------ */
/*  Presence stanza handler                                            */
/* ------------------------------------------------------------------ */

/* Dispatch a <presence> stanza received from an ONLINE session.
 * Handles:
 *   - bare <presence/>                RFC 6121 §4.2: initial presence broadcast
 *   - <presence type='unavailable'/>  RFC 6121 §4.4: unavailable broadcast
 *   - directed <presence to='...'/>   RFC 6121 §4.6: route to target
 *
 * Called from xmpp.c on_stanza when state == XMPP_STATE_ONLINE and
 * stanza name is "presence". */
void xmpp_presence_handle(xmpp_session_t* ctx, xmpp_stanza_t* stanza);

/* Broadcast unavailable presence on behalf of a session that
 * disconnected without sending <presence type='unavailable'/>.
 * Safe to call even if the session was never registered (no-op). */
void xmpp_presence_on_disconnect(const char* bound_jid, const char* bare_jid);

#endif /* XMPP_PRESENCE_H */
