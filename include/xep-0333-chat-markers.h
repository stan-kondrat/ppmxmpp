#ifndef XEP_0333_CHAT_MARKERS_H
#define XEP_0333_CHAT_MARKERS_H

#include "xmpp.h"
#include "strophe.h"

/* ------------------------------------------------------------------ */
/*  XEP-0333: Chat Markers                                              */
/*                                                                     */
/*  Allows clients to mark messages with:                              */
/*    <markable xmlns='urn:xmpp:chat-markers:0'/>                       */
/*    <received xmlns='urn:xmpp:chat-markers:0' id='msg-id'/>          */
/*    <displayed xmlns='urn:xmpp:chat-markers:0' id='msg-id'/>          */
/*    <acknowledged xmlns='urn:xmpp:chat-markers:0' id='msg-id'/>      */
/*                                                                     */
/*  The server routes marker messages like any other <message> stanza. */
/*  No new storage or special routing is required.                     */
/* ------------------------------------------------------------------ */

/* Check whether stanza contains a chat-marker child element and
 * return the marker name ("displayed", "received", "acknowledged")
 * or NULL if no marker is present.
 * Marker namespace: urn:xmpp:chat-markers:0
 *
 * Note: XEP-0333 v1.0.0 (2024-04-17) removed <received/> and <acknowledged/>
 * from the spec, leaving only <displayed/>.  The older types are still
 * recognized for compatibility with pre-v1.0.0 clients.
 *
 * The return value points into the stanza's children list; do not free it.
 * If a marker is found, marker_id_out (if non-NULL) is set to the value of
 * the marker's 'id' attribute (caller must NOT free it). */
const char* xep0333_get_marker(const xmpp_stanza_t* stanza,
                               const char** marker_id_out);

/* Check whether stanza contains a <markable xmlns='urn:xmpp:chat-markers:0'/>
 * child element.
 *
 * A <markable/> element signals that the sending client is interested in
 * receiving Displayed Markers for this message (XEP-0333 §2).
 *
 * Returns 1 if <markable/> is present, 0 otherwise. */
int xep0333_has_markable(const xmpp_stanza_t* stanza);

#endif /* XEP_0333_CHAT_MARKERS_H */