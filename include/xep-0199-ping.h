#ifndef XEP_0199_PING_H
#define XEP_0199_PING_H

#include "xmpp.h"

/* XEP-0199: respond to urn:xmpp:ping get IQ with empty result. */
void xep0199_handle_ping(xmpp_session_t* ctx, const char* iq_id);

#endif /* XEP_0199_PING_H */
