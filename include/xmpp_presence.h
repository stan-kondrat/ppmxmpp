#ifndef XMPP_PRESENCE_H
#define XMPP_PRESENCE_H

#include <stddef.h>

#include "xmpp.h"

/* Opaque libstrophe stanza type — forward-declared to avoid including strophe.h here. */
typedef struct _xmpp_stanza_t xmpp_stanza_t;

/* ------------------------------------------------------------------ */
/*  Session registry                                                   */
/*                                                                     */
/*  Tracks every authenticated, bound resource as an "available"      */
/*  session keyed by full JID.  The registry is not thread-safe; it   */
/*  is designed for the single-threaded libuv event loop.             */
/* ------------------------------------------------------------------ */

/* Register a session after successful resource bind.
 * write_fn/write_ud are the transport callbacks used to deliver
 * stanzas to this session. */
void presence_session_register(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* write_ud);

/* Remove a session from the registry (on clean close or disconnect).
 * Does nothing if the session was never registered or already removed. */
void presence_session_unregister(const char* bound_jid);

/* Send data directly to a registered session identified by full JID.
 * Returns 0 on success, -1 if the session is not in the registry. */
int presence_session_write(const char* bound_jid, const char* data, size_t len);

/* ------------------------------------------------------------------ */
/*  Presence stanza handler                                            */
/* ------------------------------------------------------------------ */

/* Dispatch a <presence> stanza received from an ONLINE session.
 * Handles:
 *   - bare <presence/>            RFC 6121 §4.2: initial presence broadcast
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

/* Reset the session registry to empty.  TEST USE ONLY — call from cmocka
 * setup/teardown to prevent stale pointers from leaking between tests. */
void presence_session_reset_all(void);

#endif /* XMPP_PRESENCE_H */
