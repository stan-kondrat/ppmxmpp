#ifndef XMPP_SASL_H
#define XMPP_SASL_H

#include <stddef.h>
#include "xmpp.h"

/* Authenticate a SASL PLAIN exchange.
 * b64_text is the base64-encoded SASL PLAIN message from the <auth> stanza.
 * Returns 0 on success, -1 on any failure. */
int handle_sasl_plain(xmpp_session_t *ctx, const char *b64_text,
                      xmpp_write_fn write_fn, void *ud);

#endif /* XMPP_SASL_H */
