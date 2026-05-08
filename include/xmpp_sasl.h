#ifndef XMPP_SASL_H
#define XMPP_SASL_H

#include "xmpp.h"
#include <stddef.h>

typedef enum {
  SASL_OK       =  0, /* authenticated */
  SASL_FAIL     = -1, /* retryable: wrong password, unknown user */
  SASL_TERMINAL = -2, /* close immediately: malformed, forbidden localpart,
                         disabled account, authzid mismatch */
} sasl_rc_t;

/* Authenticate a SASL PLAIN exchange.
 * b64_text is the base64-encoded SASL PLAIN message from the <auth> stanza. */
sasl_rc_t handle_sasl_plain(xmpp_session_t* ctx, const char* b64_text, xmpp_write_fn write_fn,
                             void* ud);

#endif /* XMPP_SASL_H */
