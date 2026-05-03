#ifndef TEST_XMPP_HELPERS_H
#define TEST_XMPP_HELPERS_H

#include "xmpp.h"

/* Buffer to capture write output. */
extern char g_write_buf[65536];
extern size_t g_write_len;

/* Mock write function that appends to g_write_buf. */
int mock_write(void* ud, const char* data, size_t len);

/* Create a test database with a user. Returns 0 on success. */
int setup_test_db(const char** db_path_out);

/* Reset the global db_path. */
void teardown_test_db(void);

/* Find a substring in the write buffer. Returns pointer into g_write_buf or NULL. */
const char* buf_contains(const char* needle);

/* Build and base64-encode a SASL PLAIN message, feed it to ctx, return xmpp_feed rc. */
int feed_sasl_plain(xmpp_session_t* ctx, const char* authzid, const char* authcid,
                    const char* passwd);

/* Simulate a completed TLS handshake: set state to CONNECTED_TLS and mark
 * parser for reset (mirrors what server.c does after mbedtls_ssl_handshake). */
void simulate_starttls(xmpp_session_t* ctx);

#endif /* TEST_XMPP_HELPERS_H */
