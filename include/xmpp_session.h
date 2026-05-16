#ifndef XMPP_SESSION_H
#define XMPP_SESSION_H

#include <stddef.h>

#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Session table                                                      */
/*                                                                     */
/*  Tracks every authenticated, bound resource as an "available"      */
/*  session keyed by full JID.  The table is not thread-safe; it is   */
/*  designed for the single-threaded libuv event loop.                */
/* ------------------------------------------------------------------ */

/* Register a session after it sends initial presence.
 * write_fn/write_ud are the transport callbacks used to deliver
 * stanzas to this session. */
void xmpp_session_table_register(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* write_ud);

/* Remove a session from the table (on clean close or disconnect).
 * Does nothing if the session was never registered or already removed. */
void xmpp_session_table_unregister(const char* bound_jid);

/* Send data directly to a registered session identified by full JID.
 * Returns 0 on success, -1 if the session is not in the table. */
int xmpp_session_table_write(const char* bound_jid, const char* data, size_t len);

/* Update the <priority> value for a registered session. No-op if not found. */
void xmpp_session_table_update_priority(const char* bound_jid, int priority);

/* Update the last_active timestamp for a registered session. No-op if not found. */
void xmpp_session_table_touch(const char* bound_jid);

/* Find the best resource for bare_jid: highest priority, recency as tiebreak.
 * Writes the full JID into out_full_jid (size out_size).
 * Returns 0 on success, -1 if no online resource exists for that bare JID. */
int xmpp_session_table_best_resource(const char* bare_jid, char* out_full_jid, size_t out_size);

/* Deliver data to all online resources of bare_jid EXCEPT exclude_full_jid.
 * Used for self-addressed messages (RFC 6121 §8). */
void xmpp_session_table_broadcast_except(const char* bare_jid, const char* exclude_full_jid,
                                         const char* data, size_t len);

/* Returns 1 if bound_jid is currently registered, 0 otherwise. */
int xmpp_session_table_is_registered(const char* bound_jid);

/* Reset the table to empty.  TEST USE ONLY — call from cmocka
 * setup/teardown to prevent stale pointers from leaking between tests. */
void xmpp_session_table_reset_all(void);

/* ------------------------------------------------------------------ */
/*  Shared JID utility                                                 */
/* ------------------------------------------------------------------ */

/* Extract bare JID (user@domain) from a full JID (user@domain/resource).
 * Works on bare JIDs too (no-op strip). */
void xmpp_session_bare_jid(const char* full, char* out, size_t out_size);

/* ------------------------------------------------------------------ */
/*  Package-internal iterators (used by xmpp_presence.c)              */
/* ------------------------------------------------------------------ */

/* Deliver data to all online resources of target_bare. */
void xmpp_session_table_broadcast_to_bare(const char* target_bare, const char* data, size_t len);

/* Invoke cb for every session whose bare JID matches bare_jid and whose
 * full JID differs from exclude_full_jid (pass NULL to include all). */
void xmpp_session_table_for_each_resource(const char* bare_jid, const char* exclude_full_jid,
                                           void (*cb)(const char* full_jid, xmpp_write_fn write_fn,
                                                      void* write_ud, const void* ud),
                                           const void* ud);

#endif /* XMPP_SESSION_H */
