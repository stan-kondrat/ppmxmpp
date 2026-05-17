#ifndef XMPP_SASL_H
#define XMPP_SASL_H

#include <stddef.h>
#include "xmpp.h"

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

/* SCRAM-SHA-256 multi-step authentication (RFC 5802 / RFC 7677).
 * step:    1 = client-first, 2 = client-final
 * input:   base64-decoded message bytes
 * in_len:  byte count of input
 * response_out / response_cap: optional in-process response buffer
 *         (unused when write_fn is supplied)
 * write_fn: response callback
 * ud:      user data
 *
 * Returns:  0 = success, 1 = challenge sent, -1 = auth failed, -2 = terminal error.
 */
int handle_scram_sha256(xmpp_session_t* ctx, int step, const char* input, size_t in_len,
                        char* response_out, size_t response_cap,
                        void (*write_fn)(void*, const char*, size_t), void* ud);

/* Return the space-separated list of available SASL mechanism names.
 * Thread-unsafe: returns a static buffer; call only from the event loop. */
const char* sasl_available_mechanisms(void);

/* Returns 1 if the mechanism requires multi-step (challenge/response) handling. */
int sasl_is_multi_step_mechanism(const char* mechanism);

#endif /* XMPP_SASL_H */
