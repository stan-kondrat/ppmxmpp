#ifndef XEP_0280_CARBONS_H
#define XEP_0280_CARBONS_H

#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  XEP-0280: Message Carbons                                          */
/*                                                                     */
/*  Per-resource opt-in via IQ:                                        */
/*    <iq type='set'><enable xmlns='urn:xmpp:carbons:2'/></iq>        */
/*    <iq type='set'><disable xmlns='urn:xmpp:carbons:2'/></iq>       */
/*                                                                     */
/*  When routing a chat message, the sender's carbons-enabled          */
/*  resources receive a <sent/> wrapped copy and the recipient's      */
/*  carbons-enabled resources receive a <received/> wrapped copy.      */
/* ------------------------------------------------------------------ */

/* Update the carbons_enabled flag for a registered session.
 * No-op if the session is not in the table. */
void xmpp_session_table_update_carbons(const char* bound_jid, int enabled);

/* Call cb(full_jid, write_fn, write_ud, ud) for every session whose
 * bare JID matches bare_jid, whose full JID differs from exclude_full_jid,
 * and whose carbons_enabled flag is set.
 * Used to deliver carbon copies to other device instances. */
void xmpp_session_table_for_each_carbon_resource(const char* bare_jid,
                                                   const char* exclude_full_jid,
                                                   void (*cb)(const char* full_jid,
                                                              xmpp_write_fn write_fn,
                                                              void* write_ud,
                                                              const void* ud),
                                                   const void* ud);

#endif /* XEP_0280_CARBONS_H */