#ifndef XEP_0030_SERVICE_DISCOVERY_H
#define XEP_0030_SERVICE_DISCOVERY_H

#include "xmpp.h"

/* XEP-0030: respond to http://jabber.org/protocol/disco#info get IQ. */
void xep0030_handle_disco_info(xmpp_session_t* ctx, const char* iq_id, const char* to,
                               const char* node);

#endif /* XEP_0030_SERVICE_DISCOVERY_H */
