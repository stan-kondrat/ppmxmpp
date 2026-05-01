#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <string.h>

#include "test_xmpp_helpers.h"

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_xmpp_starttls_proceed(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_PLAINTEXT);
  assert_true(buf_contains("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));

  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  g_write_len = 0;
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_TLS_HANDSHAKING);
  assert_true(buf_contains("<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>"));

  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_TLS);
  assert_true(buf_contains("<stream:stream"));
  assert_true(buf_contains("<mechanism>PLAIN</mechanism>"));
  assert_true(buf_contains("<stream:features>"));

  xmpp_session_cleanup(&ctx);
}

static void test_xmpp_starttls_full_flow(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_PLAINTEXT);

  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  g_write_len = 0;
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_TLS_HANDSHAKING);
  assert_true(buf_contains("<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>"));

  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_AUTHENTICATED);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  g_write_len = 0;
  const char* auth_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  rc = xmpp_feed(&ctx, auth_restart, strlen(auth_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_RESOURCE_BOUND);
  assert_true(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));
  assert_true(buf_contains("<required/></bind>"));

  const char* bind_iq = "<iq type='set' id='bind1' xmlns='jabber:client'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                        "</iq>";
  rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_CONNECTED);
  assert_true(buf_contains("<iq type='result'"));
  assert_true(buf_contains("<jid>testuser@localhost</jid>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_starttls_bad_namespace(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_PLAINTEXT);

  const char* starttls_bad = "<starttls xmlns='wrong:namespace'/>";
  g_write_len = 0;
  rc = xmpp_feed(&ctx, starttls_bad, strlen(starttls_bad), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);
  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));
  assert_true(buf_contains("<invalid-namespace/>"));

  xmpp_session_cleanup(&ctx);
}

static void test_xmpp_starttls_parser_reset(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_PLAINTEXT);

  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  g_write_len = 0;
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_TLS_HANDSHAKING);
  assert_true(buf_contains("<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>"));

  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_AUTHENTICATED);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_xmpp_starttls_proceed),
      cmocka_unit_test(test_xmpp_starttls_full_flow),
      cmocka_unit_test(test_xmpp_starttls_bad_namespace),
      cmocka_unit_test(test_xmpp_starttls_parser_reset),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
