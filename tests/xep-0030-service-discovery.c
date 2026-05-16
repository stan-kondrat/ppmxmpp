#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_xmpp_helpers.h"
#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Server domain queries                                              */
/* ------------------------------------------------------------------ */

static void test_disco_info_returns_result(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='d1' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='d1'"));
  assert_true(buf_contains("http://jabber.org/protocol/disco#info"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_info_no_to_returns_result(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* RFC 6120: omitting 'to' is equivalent to addressing the server. */
  const char* iq = "<iq id='d0' type='get'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='d0'"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_info_contains_identity(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='d2' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<identity category='server'"));
  assert_true(buf_contains("type='im'"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_info_contains_feature_ping(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='d3' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<feature var='urn:xmpp:ping'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_info_contains_feature_roster(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='d4' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<feature var='jabber:iq:roster'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  R8: bare JID queries                                               */
/* ------------------------------------------------------------------ */

static void test_disco_info_bare_jid_existing_user(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* testuser@localhost was created by setup_test_db */
  const char* iq = "<iq id='d6' type='get' to='testuser@localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='d6'"));
  /* XEP-0030 §3.1: bare JID result SHOULD have category='account' type='registered'. */
  assert_true(buf_contains("<identity category='account' type='registered'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_info_bare_jid_nonexistent_user(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='d7' type='get' to='nobody@localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  /* XEP-0030 Security Considerations: server MUST return service-unavailable
   * for non-existent bare JIDs to prevent directory harvesting. */
  assert_true(buf_contains("<service-unavailable xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  R4: node attribute handling                                        */
/* ------------------------------------------------------------------ */

static void test_disco_info_unknown_node_returns_not_found(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='d8' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'"
                   " node='http://example.com/some-node'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Error cases                                                        */
/* ------------------------------------------------------------------ */

static void test_disco_info_unknown_entity_returns_not_found(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='d5' type='get' to='unknown.example.com'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  disco#items tests                                                 */
/* ------------------------------------------------------------------ */

static void test_disco_items_server_returns_empty_result(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='i1' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='i1'"));
  assert_true(buf_contains("http://jabber.org/protocol/disco#items"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_items_no_to_returns_result(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Omitting 'to' is equivalent to addressing the server. */
  const char* iq = "<iq id='i0' type='get'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='i0'"));
  assert_true(buf_contains("http://jabber.org/protocol/disco#items"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_items_bare_jid_existing_user_returns_empty_result(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* testuser@localhost was created by setup_test_db */
  const char* iq = "<iq id='i2' type='get' to='testuser@localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='i2'"));
  /* Empty query (no <item/> children) for user JIDs */
  assert_true(buf_contains("<query xmlns='http://jabber.org/protocol/disco#items'/>"));


  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_items_nonexistent_user_returns_service_unavailable(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='i3' type='get' to='nobody@localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  /* Security: non-existent bare JID → service-unavailable */
  assert_true(buf_contains("<service-unavailable xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_items_unknown_entity_returns_not_found(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='i4' type='get' to='unknown.example.com'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_disco_items_unknown_node_returns_not_found(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='i5' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#items'"
                   " node='http://example.com/some-node'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_disco_info_returns_result),
      cmocka_unit_test(test_disco_info_no_to_returns_result),
      cmocka_unit_test(test_disco_info_contains_identity),
      cmocka_unit_test(test_disco_info_contains_feature_ping),
      cmocka_unit_test(test_disco_info_contains_feature_roster),
      cmocka_unit_test(test_disco_info_bare_jid_existing_user),
      cmocka_unit_test(test_disco_info_bare_jid_nonexistent_user),
      cmocka_unit_test(test_disco_info_unknown_node_returns_not_found),
      cmocka_unit_test(test_disco_info_unknown_entity_returns_not_found),
      /* disco#items tests */
      cmocka_unit_test(test_disco_items_server_returns_empty_result),
      cmocka_unit_test(test_disco_items_no_to_returns_result),
      cmocka_unit_test(test_disco_items_bare_jid_existing_user_returns_empty_result),
      cmocka_unit_test(test_disco_items_nonexistent_user_returns_service_unavailable),
      cmocka_unit_test(test_disco_items_unknown_entity_returns_not_found),
      cmocka_unit_test(test_disco_items_unknown_node_returns_not_found),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
