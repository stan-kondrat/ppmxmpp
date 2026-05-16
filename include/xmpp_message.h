#ifndef XMPP_MESSAGE_H
#define XMPP_MESSAGE_H

#include "xmpp.h"

/* Opaque libstrophe stanza type. */
typedef struct _xmpp_stanza_t xmpp_stanza_t;

/* Dispatch a <message> stanza received from an ONLINE session.
 * Implements RFC 6121 §8 routing rules:
 *   - Full JID target: deliver to that exact resource if available,
 *     otherwise fall back to bare-JID routing.
 *   - Bare JID target: deliver to the resource with highest priority
 *     (recency as tiebreak). Self-addressed bare JID delivers to all
 *     other resources of the same user.
 *   - No online resource: silently dropped (offline store is Step 11).
 *
 * Called from xmpp.c on_stanza when state == XMPP_STATE_ONLINE and
 * stanza name is "message". */
void xmpp_message_handle(xmpp_session_t* ctx, xmpp_stanza_t* stanza);

#endif /* XMPP_MESSAGE_H */
