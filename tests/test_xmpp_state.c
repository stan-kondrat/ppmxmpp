#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <string.h>

#include "test_xmpp_helpers.h"
#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Helper: feed a stream:stream open and verify the resulting state   */
/* ------------------------------------------------------------------ */

static int feed_stream_open(xmpp_session_t* ctx, const char* to_domain) {
  char buf[2048];
  if (to_domain) {
    snprintf(buf, sizeof(buf),
             "<?xml version='1.0'?>"
             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
             "xmlns='jabber:client' to='%s' version='1.0' xml:lang='en'>",
             to_domain);
  } else {
    snprintf(buf, sizeof(buf),
             "<?xml version='1.0'?>"
             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
             "xmlns='jabber:client' version='1.0' xml:lang='en'>");
  }
  return xmpp_feed(ctx, buf, strlen(buf), mock_write, NULL);
}

/* ------------------------------------------------------------------ */
/*  1. Stream opens in CONNECTED state                                */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_initial_connected(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* After reset, the session should be in CONNECTED state. */
  assert_int_equal(ctx.state, XMPP_STATE_CONNECTED_TCP);

  /* No output has been produced yet. */
  assert_int_equal(g_write_len, 0);

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  2. Plaintext features only show STARTTLS                          */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_plaintext_features_only_starttls(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Verify stream:features contains STARTTLS with <required/>. */
  assert_true(buf_contains("<stream:features>"));
  assert_true(buf_contains("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));
  assert_true(buf_contains("<required/>"));

  /* Verify SASL mechanisms are NOT present in plaintext features. */
  assert_false(buf_contains("<mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));

  /* Verify bind is NOT present in plaintext features. */
  assert_false(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  3. STARTTLS required: auth before STARTTLS produces stream error  */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_auth_before_starttls(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Send SASL auth in plaintext — should produce policy-violation. */
  const char* auth_xml = "<auth mechanism='PLAIN' xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                         "dGVzdHVzZXJAZG9tYWluLmNvbQB0ZXN0dXNlcgB0ZXN0cGFzcw=="
                         "</auth>";
  rc = xmpp_feed(&ctx, auth_xml, strlen(auth_xml), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  /* Should contain stream:error with policy-violation. */
  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<policy-violation xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  4. Post-TLS features show SASL mechanisms                         */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_post_tls_features_show_sasl(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Step 1: Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Step 2: Client requests STARTTLS. */
  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);
  assert_true(buf_contains("<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>"));

  /* Step 3: Client restarts stream (simulating post-TLS). */
  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  /* Verify stream:features contains SASL mechanisms. */
  assert_true(buf_contains("<stream:features>"));
  assert_true(buf_contains("<mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));
  assert_true(buf_contains("<mechanism>PLAIN</mechanism>"));
  assert_true(buf_contains("</mechanisms>"));

  /* Verify STARTTLS is NOT present after TLS. */
  assert_false(buf_contains("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));

  /* Verify bind is NOT present yet. */
  assert_false(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  5. Post-SASL features show bind                                   */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_post_sasl_features_show_bind(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Step 1: Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Step 2: STARTTLS. */
  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  /* Step 3: Stream restart after TLS. */
  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  /* Step 4: SASL PLAIN auth. */
  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  /* Step 5: Stream restart after SASL. */
  g_write_len = 0;
  const char* auth_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  rc = xmpp_feed(&ctx, auth_restart, strlen(auth_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_BOUND);

  /* Verify stream:features contains bind. */
  assert_true(buf_contains("<stream:features>"));
  assert_true(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));
  assert_true(buf_contains("<required/></bind>"));

  /* Verify SASL mechanisms are NOT present after SASL. */
  assert_false(buf_contains("<mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));

  /* Verify STARTTLS is NOT present. */
  assert_false(buf_contains("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  6. Bind before SASL produces stream error                         */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_bind_before_sasl(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Send bind IQ without SASL — should produce policy-violation. */
  const char* bind_iq = "<iq type='set' id='bind1' xmlns='jabber:client'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                        "</iq>";
  rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  /* Should contain stream:error with policy-violation. */
  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<policy-violation xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  7. Out-of-order stanza produces stream error                      */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_out_of_order_stanza(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Send an unexpected stanza (presence) in plaintext — should produce
   * policy-violation since only <starttls/> is expected. */
  const char* presence = "<presence type='available'/>";
  rc = xmpp_feed(&ctx, presence, strlen(presence), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<policy-violation xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  8. Out-of-order stanza in TLS_HANDSHAKING state                   */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_out_of_order_in_tls_state(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Request STARTTLS. */
  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  /* Send an unexpected stanza (presence) in TLS state — should produce
   * not-well-formed since only <auth/> is expected. */
  const char* presence = "<presence type='available'/>";
  rc = xmpp_feed(&ctx, presence, strlen(presence), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<not-well-formed xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  9. Out-of-order stanza in STREAM_OPENED_AUTHENTICATED state       */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_out_of_order_in_authenticated_state(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* STARTTLS. */
  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  /* Stream restart after TLS. */
  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  /* SASL PLAIN auth. */
  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);

  /* Send an unexpected stanza (presence) in authenticated state —
   * should produce not-well-formed since only bind IQ is expected. */
  const char* presence = "<presence type='available'/>";
  rc = xmpp_feed(&ctx, presence, strlen(presence), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<not-well-formed xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  10. Graceful close flow                                           */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_graceful_close(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Client sends </stream:stream> to close gracefully. */
  g_write_len = 0;
  const char* close_stream = "</stream:stream>";
  rc = xmpp_feed(&ctx, close_stream, strlen(close_stream), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  /* Server should respond with </stream:stream>. */
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  11. Graceful close from RESOURCE_BOUND state                      */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_graceful_close_from_bound(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Full flow to RESOURCE_BOUND. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);

  g_write_len = 0;
  const char* auth_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  rc = xmpp_feed(&ctx, auth_restart, strlen(auth_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_BOUND);

  /* Client sends </stream:stream> from RESOURCE_BOUND. */
  g_write_len = 0;
  const char* close_stream = "</stream:stream>";
  rc = xmpp_feed(&ctx, close_stream, strlen(close_stream), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  12. Full STARTTLS + SASL + bind flow (end-to-end)                 */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_full_starttls_sasl_bind_flow(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Phase 1: CONNECTED -> STREAM_OPENED_PLAINTEXT */
  int rc = feed_stream_open(&ctx, "example.com");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);
  assert_true(buf_contains("from='example.com'"));
  assert_true(buf_contains("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));

  /* Phase 2: STREAM_OPENED_PLAINTEXT -> TLS_HANDSHAKING */
  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);
  assert_true(buf_contains("<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>"));

  /* Phase 3: TLS_HANDSHAKING -> STREAM_OPENED_TLS (stream restart) */
  g_write_len = 0;
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='example.com' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);
  assert_true(buf_contains("<mechanism>PLAIN</mechanism>"));

  /* Phase 4: STREAM_OPENED_TLS -> SASL_AUTHENTICATING -> STREAM_OPENED_AUTHENTICATED */
  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  /* Phase 5: STREAM_OPENED_AUTHENTICATED -> RESOURCE_BOUND (stream restart) */
  g_write_len = 0;
  const char* auth_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='example.com' version='1.0' "
                             "xml:lang='en'>";
  rc = xmpp_feed(&ctx, auth_restart, strlen(auth_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_BOUND);
  assert_true(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));
  assert_true(buf_contains("<required/></bind>"));

  /* Phase 6: RESOURCE_BOUND via bind IQ */
  const char* bind_iq = "<iq type='set' id='bind1' xmlns='jabber:client'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                        "</iq>";
  rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_ONLINE);
  assert_true(buf_contains("<iq type='result'"));
  assert_true(buf_contains("<jid>testuser@example.com/"));
  assert_true(buf_contains("</jid>"));

  /* Phase 7: Graceful close */
  g_write_len = 0;
  const char* close_stream = "</stream:stream>";
  rc = xmpp_feed(&ctx, close_stream, strlen(close_stream), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  13. Stream error on bad namespace in <stream:stream>              */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_bad_namespace(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Send stream:stream with wrong client namespace. */
  const char* bad_ns = "<?xml version='1.0'?>"
                       "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                       "xmlns='wrong:namespace' to='localhost' version='1.0' xml:lang='en'>";
  int rc = xmpp_feed(&ctx, bad_ns, strlen(bad_ns), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<invalid-namespace xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  14. Stream error on bad version                                   */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_bad_version(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Send stream:stream with wrong version. */
  const char* bad_ver = "<?xml version='1.0'?>"
                        "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                        "xmlns='jabber:client' to='localhost' version='0.9' xml:lang='en'>";
  int rc = xmpp_feed(&ctx, bad_ver, strlen(bad_ver), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<not-well-formed xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  15. Multiple stream restarts (CONNECTED -> ... -> CONNECTED)      */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_multiple_stream_restarts(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* First connection: full flow. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  g_write_len = 0;
  const char* restart1 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                         "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart1, strlen(restart1), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);

  g_write_len = 0;
  const char* restart2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                         "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart2, strlen(restart2), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_BOUND);

  const char* bind_iq = "<iq type='set' id='bind1' xmlns='jabber:client'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                        "</iq>";
  rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_ONLINE);

  /* Second connection: reset and start fresh. */
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  assert_int_equal(ctx.state, XMPP_STATE_CONNECTED_TCP);

  rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);
  assert_true(buf_contains("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  16. Stream error on unsupported mechanism                         */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_unsupported_mechanism(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Send auth with unsupported mechanism in plaintext. */
  const char* auth_xml = "<auth mechanism='DIGEST-MD5' xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>";
  rc = xmpp_feed(&ctx, auth_xml, strlen(auth_xml), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSED);

  assert_true(buf_contains("<unsupported-mechanism/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  17. Stream error on non-bind IQ in authenticated state            */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_non_bind_iq_in_authenticated(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Full flow to authenticated state. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  g_write_len = 0;
  const char* restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                        "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart, strlen(restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);

  /* Send a non-bind IQ (e.g., get IQ) — should produce not-well-formed. */
  const char* get_iq = "<iq type='get' id='get1' xmlns='jabber:client'>"
                       "<query xmlns='jabber:iq:roster'/>"
                       "</iq>";
  rc = xmpp_feed(&ctx, get_iq, strlen(get_iq), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<not-well-formed xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  18. Stream error on non-set IQ type in authenticated state        */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_non_set_iq_in_authenticated(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Full flow to authenticated state. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  g_write_len = 0;
  const char* restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                        "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart, strlen(restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);

  /* Send an IQ with type='get' (not 'set') — should produce not-well-formed. */
  const char* get_iq = "<iq type='get' id='get1' xmlns='jabber:client'>"
                       "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                       "</iq>";
  rc = xmpp_feed(&ctx, get_iq, strlen(get_iq), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<not-well-formed xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  19. Stream error on IQ without bind child in authenticated state  */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_iq_without_bind_in_authenticated(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Full flow to authenticated state. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  rc = xmpp_feed(&ctx, starttls, strlen(starttls), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_STARTTLS_SENT);

  g_write_len = 0;
  const char* restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                        "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  rc = xmpp_feed(&ctx, restart, strlen(restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);

  /* Send an IQ with type='set' but no <bind> child — should produce
   * not-well-formed. */
  const char* bad_iq = "<iq type='set' id='bad1' xmlns='jabber:client'>"
                       "<query xmlns='jabber:iq:register'/>"
                       "</iq>";
  rc = xmpp_feed(&ctx, bad_iq, strlen(bad_iq), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<stream:error>"));
  assert_true(buf_contains("<not-well-formed xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  20. Stream error on bad TLS namespace                             */
/* ------------------------------------------------------------------ */

static void test_xmpp_state_bad_tls_namespace(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream in plaintext. */
  int rc = feed_stream_open(&ctx, "localhost");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* Send STARTTLS with wrong namespace. */
  const char* bad_starttls = "<starttls xmlns='wrong:namespace'/>";
  rc = xmpp_feed(&ctx, bad_starttls, strlen(bad_starttls), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_CLOSING);

  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"));
  assert_true(buf_contains("<invalid-namespace/>"));
  assert_true(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
}

/* ------------------------------------------------------------------ */
/*  Bind tests helper                                                 */
/* ------------------------------------------------------------------ */

/* Drive session from CONNECTED all the way to XMPP_STATE_BOUND.
 * Requires an active test DB (call setup_test_db first). */
static int feed_to_resource_bound(xmpp_session_t* ctx) {
  xmpp_session_reset(ctx);
  g_write_len = 0;

  if (feed_stream_open(ctx, "example.com") != 0) {
    return -1;
  }
  if (ctx->state != XMPP_STATE_FEATURES_RECEIVED) {
    return -1;
  }

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  if (xmpp_feed(ctx, starttls, strlen(starttls), mock_write, NULL) != 0) {
    return -1;
  }
  if (ctx->state != XMPP_STATE_STARTTLS_SENT) {
    return -1;
  }
  if (!ctx->needs_starttls_proceed) {
    return -1;
  }
  simulate_starttls(ctx);

  g_write_len = 0;
  const char* restart1 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                         "xmlns='jabber:client' to='example.com' version='1.0'>";
  if (xmpp_feed(ctx, restart1, strlen(restart1), mock_write, NULL) != 0) {
    return -1;
  }
  if (ctx->state != XMPP_STATE_FEATURES_RECEIVED_POST_TLS) {
    return -1;
  }

  if (feed_sasl_plain(ctx, "", "testuser", "testpass") != 0) {
    return -1;
  }
  if (ctx->state != XMPP_STATE_SASL_SUCCESS) {
    return -1;
  }

  g_write_len = 0;
  const char* restart2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                         "xmlns='jabber:client' to='example.com' version='1.0'>";
  if (xmpp_feed(ctx, restart2, strlen(restart2), mock_write, NULL) != 0) {
    return -1;
  }
  if (ctx->state != XMPP_STATE_BOUND) {
    return -1;
  }

  g_write_len = 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  21. Bind with client-supplied resource                            */
/* ------------------------------------------------------------------ */

static void test_bind_with_resource(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_resource_bound(&ctx), 0);

  const char* bind_iq = "<iq type='set' id='b1'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                        "<resource>laptop</resource>"
                        "</bind></iq>";
  int rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_ONLINE);
  assert_true(buf_contains("<jid>testuser@example.com/laptop</jid>"));
  assert_string_equal(ctx.resource, "laptop");
  assert_string_equal(ctx.bound_jid, "testuser@example.com/laptop");

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  22. Bind without resource — server generates 8-hex resource       */
/* ------------------------------------------------------------------ */

static void test_bind_without_resource_generates_jid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_resource_bound(&ctx), 0);

  const char* bind_iq = "<iq type='set' id='b2'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                        "</iq>";
  int rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_ONLINE);

  assert_int_equal((int)strlen(ctx.resource), 8);
  for (int i = 0; i < 8; i++) {
    char ch = ctx.resource[i];
    assert_true((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
  }
  assert_true(buf_contains("<jid>testuser@example.com/"));
  assert_true(buf_contains("</jid>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  23. Bind with oversized resource — bad-request, state unchanged   */
/* ------------------------------------------------------------------ */

static void test_bind_oversized_resource(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_resource_bound(&ctx), 0);

  /* Build a 1024-char resource (one over the 1023-byte limit). */
  char big[4096];
  snprintf(big, sizeof(big),
           "<iq type='set' id='b3'>"
           "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
           "<resource>%.*s</resource>"
           "</bind></iq>",
           1024,
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  int rc = xmpp_feed(&ctx, big, strlen(big), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_BOUND);
  assert_true(buf_contains("<iq type='error' id='b3'>"));
  assert_true(buf_contains("<bad-request"));
  assert_int_equal((int)ctx.resource[0], 0);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  24. Bind with forbidden character in resource — bad-request       */
/* ------------------------------------------------------------------ */

static void test_bind_forbidden_char_in_resource(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_resource_bound(&ctx), 0);

  const char* bind_iq = "<iq type='set' id='b4'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                        "<resource>bad/resource</resource>"
                        "</bind></iq>";
  int rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_BOUND);
  assert_true(buf_contains("<bad-request"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  25. Bind with no id attribute — result omits id                   */
/* ------------------------------------------------------------------ */

static void test_bind_no_id_attribute(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_resource_bound(&ctx), 0);

  const char* bind_iq = "<iq type='set'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                        "<resource>phone</resource>"
                        "</bind></iq>";
  int rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_ONLINE);
  assert_true(buf_contains("<jid>testuser@example.com/phone</jid>"));
  assert_string_equal(ctx.bound_jid, "testuser@example.com/phone");

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      /* State initialization */
      cmocka_unit_test(test_xmpp_state_initial_connected),

      /* Features advertisement per state */
      cmocka_unit_test(test_xmpp_state_plaintext_features_only_starttls),
      cmocka_unit_test(test_xmpp_state_post_tls_features_show_sasl),
      cmocka_unit_test(test_xmpp_state_post_sasl_features_show_bind),

      /* Out-of-order stanza handling */
      cmocka_unit_test(test_xmpp_state_auth_before_starttls),
      cmocka_unit_test(test_xmpp_state_bind_before_sasl),
      cmocka_unit_test(test_xmpp_state_out_of_order_stanza),
      cmocka_unit_test(test_xmpp_state_out_of_order_in_tls_state),
      cmocka_unit_test(test_xmpp_state_out_of_order_in_authenticated_state),

      /* Graceful close */
      cmocka_unit_test(test_xmpp_state_graceful_close),
      cmocka_unit_test(test_xmpp_state_graceful_close_from_bound),

      /* End-to-end flow */
      cmocka_unit_test(test_xmpp_state_full_starttls_sasl_bind_flow),

      /* Stream validation errors */
      cmocka_unit_test(test_xmpp_state_bad_namespace),
      cmocka_unit_test(test_xmpp_state_bad_version),

      /* Session reset and reuse */
      cmocka_unit_test(test_xmpp_state_multiple_stream_restarts),

      /* Mechanism validation */
      cmocka_unit_test(test_xmpp_state_unsupported_mechanism),

      /* IQ validation in authenticated state */
      cmocka_unit_test(test_xmpp_state_non_bind_iq_in_authenticated),
      cmocka_unit_test(test_xmpp_state_non_set_iq_in_authenticated),
      cmocka_unit_test(test_xmpp_state_iq_without_bind_in_authenticated),

      /* TLS namespace validation */
      cmocka_unit_test(test_xmpp_state_bad_tls_namespace),

      /* Resource binding */
      cmocka_unit_test(test_bind_with_resource),
      cmocka_unit_test(test_bind_without_resource_generates_jid),
      cmocka_unit_test(test_bind_oversized_resource),
      cmocka_unit_test(test_bind_forbidden_char_in_resource),
      cmocka_unit_test(test_bind_no_id_attribute),
  };
  return cmocka_run_group_tests(tests, log_group_setup, log_group_teardown);
}
