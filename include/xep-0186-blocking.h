#ifndef XEP_0186_BLOCKING_H
#define XEP_0186_BLOCKING_H

#include "xmpp_iq_dispatch.h"

/* Initialize XEP-0186 blocking handlers.
 * Registers IQ handlers for jabber:iq:privacy.
 * Returns 0 on success, -1 on error. */
int xep0186_init(void);

#endif /* XEP_0186_BLOCKING_H */