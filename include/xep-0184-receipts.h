#ifndef XEP_0184_RECEIPTS_H
#define XEP_0184_RECEIPTS_H

#include "xmpp.h"
#include "strophe.h"

/* ------------------------------------------------------------------ */
/*  XEP-0184: Message Delivery Receipts                                */
/*                                                                     */
/*  Namespace: urn:xmpp:receipts                                       */
/*                                                                     */
/*  Features:                                                          */
/*    - Auto-detect <request xmlns='urn:xmpp:receipts'/> in incoming  */
/*      chat messages and send back <received id='orig_id'/> receipt. */
/*    - Exclude <received/> and <displayed/> ack stanzas from carbon   */
/*      copy forwarding (XEP-0280 §9 analogy).                         */
/*    - Advertise 'urn:xmpp:receipts' feature in disco#info.          */
/*                                                                     */
/*  No new storage required.                                          */
/* ------------------------------------------------------------------ */

/* XEP-0184 namespace. */
#define XEP0184_NS "urn:xmpp:receipts"

/* Initialize XEP-0184: register disco feature.
 * Called during server startup. */
int xep0184_init(void);

/* Check whether a message stanza contains a receipt <request/> element.
 * Returns 1 if found, 0 otherwise. */
int xep0184_has_request(const xmpp_stanza_t* stanza);

/* Check whether a message stanza contains a <received/> receipt element.
 * Returns 1 if found, 0 otherwise. */
int xep0184_has_received(const xmpp_stanza_t* stanza);

/* Check whether a message stanza contains a <displayed/> receipt element.
 * Returns 1 if found, 0 otherwise. */
int xep0184_has_displayed(const xmpp_stanza_t* stanza);

/* Send a delivery receipt back to the sender.
 * - ctx:        session of the recipient (used for write_fn)
 * - from_jid:   recipient's full JID (the server address as from=)
 * - to_jid:     original sender's JID (to=)
 * - orig_id:    id= from the original content message
 * Returns 0 on success, -1 on error. */
int xep0184_send_receipt(xmpp_session_t* ctx, const char* from_jid,
                          const char* to_jid, const char* orig_id);

/* Send a delivery receipt with <displayed/> instead of <received/>.
 * Same semantics but uses <displayed xmlns='urn:xmpp:receipts'/>. */
int xep0184_send_displayed(xmpp_session_t* ctx, const char* from_jid,
                            const char* to_jid, const char* orig_id);

#endif /* XEP_0184_RECEIPTS_H */