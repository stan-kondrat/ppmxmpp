#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "storage/db.h"
#include "test_xmpp_helpers.h"

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_xmpp_stream_negotiation(void** state) {
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

  assert_true(buf_contains("<?xml version='1.0'"));
  assert_true(buf_contains("<stream:stream"));
  assert_true(buf_contains("from='localhost'"));
  assert_true(buf_contains("id='"));
  assert_true(buf_contains("<stream:features>"));
  assert_true(buf_contains("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));
  xmpp_session_cleanup(&ctx);
}

static void test_xmpp_sasl_plain_success(void** state) {
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

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_TLS_HANDSHAKING);

  g_write_len = 0;
  const char* restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                         "xmlns='jabber:client' to='localhost' version='1.0' "
                         "xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart, strlen(restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_AUTHENTICATED);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_sasl_plain_bad_password(void** state) {
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

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);

  g_write_len = 0;
  const char* restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                         "xmlns='jabber:client' to='localhost' version='1.0' "
                         "xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart, strlen(restart), mock_write, NULL);
  assert_int_equal(rc, 0);

   rc = feed_sasl_plain(&ctx, "", "testuser", "wrongpass");
   assert_int_equal(rc, 0);
   assert_int_equal(ctx.state, XMPP_STATE_CLOSING);
  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_sasl_plain_user_not_found(void** state) {
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

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);

  g_write_len = 0;
  const char* restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                         "xmlns='jabber:client' to='localhost' version='1.0' "
                         "xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart, strlen(restart), mock_write, NULL);
  assert_int_equal(rc, 0);

   rc = feed_sasl_plain(&ctx, "", "nonexistent", "anypass");
   assert_int_equal(rc, 0);
   assert_int_equal(ctx.state, XMPP_STATE_CLOSING);
  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_full_connection_flow(void** state) {
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

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_AUTHENTICATED);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                                "xmlns='jabber:client' to='localhost' version='1.0' "
                                "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
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
  assert_true(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));
  assert_true(buf_contains("<jid>testuser@localhost</jid>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_unsupported_mechanism(void** state) {
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

  const char* auth_xml = "<auth mechanism='DIGEST-MD5' xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>";
  rc = xmpp_feed(&ctx, auth_xml, strlen(auth_xml), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSED);

  assert_true(buf_contains("<unsupported-mechanism/>"));
  xmpp_session_cleanup(&ctx);
}

static void test_xmpp_disabled_account(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  char sql[512];
  snprintf(sql, sizeof(sql), "UPDATE users SET disabled = 1 WHERE jid = 'testuser@localhost'");
  sqlite3* db;
  int rc = sqlite3_open(server_config.db_path, &db);
  if (rc == SQLITE_OK) {
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
  }

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                              "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                              "xmlns='jabber:client' to='localhost' version='1.0' "
                              "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);

   rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
   assert_int_equal(rc, 0);
   assert_int_equal(ctx.state, XMPP_STATE_CLOSING);
  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_incomplete_elements(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* chunk1 = "<?xml version='1.0'?><stream:stream "
                        "xmlns:stream='http://etherx.jabber.org/streams' xmlns='jabber:client' "
                        "to='localhost' version='1.0' xml:lang='en'>";
  int rc = xmpp_feed(&ctx, chunk1, strlen(chunk1), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_PLAINTEXT);

  assert_true(buf_contains("<stream:features>"));
  xmpp_session_cleanup(&ctx);
}

static void test_xmpp_domain_extraction(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                              "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                              "xmlns='jabber:client' to='example.com' version='1.0' "
                              "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_PLAINTEXT);

  assert_true(buf_contains("from='example.com'"));
  xmpp_session_cleanup(&ctx);
}

static void test_xmpp_default_domain(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                              "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                              "xmlns='jabber:client' version='1.0' "
                              "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_PLAINTEXT);

  assert_true(buf_contains("from='localhost'"));
  xmpp_session_cleanup(&ctx);
}

static void test_xmpp_sasl_authzid_match(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* hello = "<?xml version='1.0'?>"
                      "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                      "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  assert_int_equal(xmpp_feed(&ctx, hello, strlen(hello), mock_write, NULL), 0);

  int rc = feed_sasl_plain(&ctx, "testuser@localhost", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STREAM_OPENED_AUTHENTICATED);
  assert_true(buf_contains("<success"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_sasl_authzid_mismatch(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* hello = "<?xml version='1.0'?>"
                      "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                      "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  assert_int_equal(xmpp_feed(&ctx, hello, strlen(hello), mock_write, NULL), 0);

  int rc = feed_sasl_plain(&ctx, "other@localhost", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_xmpp_sasl_forbidden_localpart(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* hello = "<?xml version='1.0'?>"
                      "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                      "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  assert_int_equal(xmpp_feed(&ctx, hello, strlen(hello), mock_write, NULL), 0);

  int rc = feed_sasl_plain(&ctx, "", "user@name", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_xmpp_stream_negotiation),
      cmocka_unit_test(test_xmpp_sasl_plain_success),
      cmocka_unit_test(test_xmpp_sasl_plain_bad_password),
      cmocka_unit_test(test_xmpp_sasl_plain_user_not_found),
      cmocka_unit_test(test_xmpp_full_connection_flow),
      cmocka_unit_test(test_xmpp_unsupported_mechanism),
      cmocka_unit_test(test_xmpp_disabled_account),
      cmocka_unit_test(test_xmpp_incomplete_elements),
      cmocka_unit_test(test_xmpp_domain_extraction),
      cmocka_unit_test(test_xmpp_default_domain),
      cmocka_unit_test(test_xmpp_sasl_authzid_match),
      cmocka_unit_test(test_xmpp_sasl_authzid_mismatch),
      cmocka_unit_test(test_xmpp_sasl_forbidden_localpart),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
