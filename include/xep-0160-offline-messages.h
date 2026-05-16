#ifndef XEP_0160_OFFLINE_MESSAGES_H
#define XEP_0160_OFFLINE_MESSAGES_H

#include <stddef.h>

#include "xmpp_session.h"

/* XEP-0160: Best Practices for Handling Offline Messages
 *
 * Store a pre-built message stanza for an offline recipient.
 * Sends a service-unavailable error to the sender if the storage cap is
 * reached.  Returns 0 on success, -1 on error, -2 if cap reached. */
int xep0160_store(xmpp_session_t* ctx, const char* to_bare, const char* msg_id,
                  const char* stanza_xml, size_t stanza_len);

#endif /* XEP_0160_OFFLINE_MESSAGES_H */
