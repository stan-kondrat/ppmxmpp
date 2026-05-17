#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db_users.h"
#include "storage/db_vcard.h"
#include "test_xmpp_helpers.h"
#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Own vCard: get (empty)                                             */
/* ------------------------------------------------------------------ */

static void test_vcard_get_own_empty_returns_empty_vcard(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* No vCard stored yet → server returns empty <vCard/> */
  const char* iq = "<iq id='v1' type='get'>"
                   "<vCard xmlns='vcard-temp'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v1'"));
  assert_true(buf_contains("<vCard xmlns='vcard-temp'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Own vCard: set then get                                             */
/* ------------------------------------------------------------------ */

static void test_vcard_set_own_then_get_returns_stored_vcard(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  char iq[2048];
  snprintf(iq, sizeof(iq),
           "<iq id='v2' type='set'>"
           "<vCard xmlns='vcard-temp'>"
           "<FN>Test User</FN>"
           "<N><FAMILY>User</FAMILY><GIVEN>Test</GIVEN></N>"
           "<NICKNAME>testuser</NICKNAME>"
           "</vCard>"
           "</iq>");
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v2'/>"));

  /* Retrieve own vCard */
  reset_write_buf();
  const char* get_iq = "<iq id='v3' type='get'>"
                       "<vCard xmlns='vcard-temp'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, get_iq, strlen(get_iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v3'"));
  assert_true(buf_contains("<vCard xmlns='vcard-temp'>"));
  assert_true(buf_contains("<FN>Test User</FN>"));
  assert_true(buf_contains("<NICKNAME>testuser</NICKNAME>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Set with 'to' pointing to own bare JID succeeds                     */
/* ------------------------------------------------------------------ */

static void test_vcard_set_with_to_own_bare_jid_succeeds(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  char iq[2048];
  snprintf(iq, sizeof(iq),
           "<iq id='v4' type='set' to='testuser@localhost'>"
           "<vCard xmlns='vcard-temp'>"
           "<FN>Overwritten</FN>"
           "</vCard>"
           "</iq>");
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v4'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Set with 'to' pointing to another user → forbidden                  */
/* ------------------------------------------------------------------ */

static void test_vcard_set_to_other_user_returns_forbidden(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Register bob so we can target his JID */
  (void)storage_users_create("bob@localhost", "testpass");

  char iq[2048];
  snprintf(iq, sizeof(iq),
           "<iq id='v5' type='set' to='bob@localhost'>"
           "<vCard xmlns='vcard-temp'>"
           "<FN>Trying to set bob's vCard</FN>"
           "</vCard>"
           "</iq>");
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  /* XEP-0054 §"Updating One's vCard": server MUST return forbidden */
  assert_true(buf_contains("<forbidden xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Get another user's vCard (non-existent user → service-unavailable)  */
/* ------------------------------------------------------------------ */

static void test_vcard_get_nonexistent_user_returns_service_unavailable(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* No such user → server returns service-unavailable (prevents enumeration) */
  const char* iq = "<iq id='v6' type='get' to='nobody@localhost'>"
                   "<vCard xmlns='vcard-temp'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<service-unavailable xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Get another user's vCard (existing user with no vCard → empty)      */
/* ------------------------------------------------------------------ */

static void test_vcard_get_other_existing_user_no_vcard_returns_empty(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Register bob but don't store his vCard */
  (void)storage_users_create("bob@localhost", "testpass");

  const char* iq = "<iq id='v7' type='get' to='bob@localhost'>"
                   "<vCard xmlns='vcard-temp'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v7'"));
  assert_true(buf_contains("<vCard xmlns='vcard-temp'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Get another user's vCard (existing user with stored vCard)          */
/* ------------------------------------------------------------------ */

static void test_vcard_get_other_user_with_stored_vcard(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Manually store bob's vCard using the storage API */
  assert_int_equal(storage_vcard_set("bob@localhost",
                                     "<FN>Bob The Builder</FN><NICKNAME>bob</NICKNAME>"),
                   0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='v8' type='get' to='bob@localhost'>"
                   "<vCard xmlns='vcard-temp'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v8'"));
  assert_true(buf_contains("<FN>Bob The Builder</FN>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Get foreign domain → item-not-found                                  */
/* ------------------------------------------------------------------ */

static void test_vcard_get_foreign_domain_returns_not_found(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='v9' type='get' to='user@foreign.example.com'>"
                   "<vCard xmlns='vcard-temp'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Update existing vCard (replace)                                      */
/* ------------------------------------------------------------------ */

static void test_vcard_update_existing_replaces_previous(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Set initial vCard */
  char iq1[1024];
  snprintf(iq1, sizeof(iq1),
           "<iq id='v10' type='set'>"
           "<vCard xmlns='vcard-temp'>"
           "<FN>First Name</FN>"
           "</vCard></iq>");
  assert_int_equal(xmpp_feed(&ctx, iq1, strlen(iq1), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v10'/>"));

  /* Replace with new vCard */
  reset_write_buf();
  char iq2[1024];
  snprintf(iq2, sizeof(iq2),
           "<iq id='v11' type='set'>"
           "<vCard xmlns='vcard-temp'>"
           "<FN>Second Name</FN>"
           "</vCard></iq>");
  assert_int_equal(xmpp_feed(&ctx, iq2, strlen(iq2), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='v11'/>"));

  /* Verify only the new vCard is returned */
  reset_write_buf();
  const char* get_iq = "<iq id='v12' type='get'>"
                       "<vCard xmlns='vcard-temp'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, get_iq, strlen(get_iq), mock_write, NULL), 0);
  assert_true(buf_contains("<FN>Second Name</FN>"));
  assert_false(buf_contains("First Name"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_vcard_get_own_empty_returns_empty_vcard),
      cmocka_unit_test(test_vcard_set_own_then_get_returns_stored_vcard),
      cmocka_unit_test(test_vcard_set_with_to_own_bare_jid_succeeds),
      cmocka_unit_test(test_vcard_set_to_other_user_returns_forbidden),
      cmocka_unit_test(test_vcard_get_nonexistent_user_returns_service_unavailable),
      cmocka_unit_test(test_vcard_get_other_existing_user_no_vcard_returns_empty),
      cmocka_unit_test(test_vcard_get_other_user_with_stored_vcard),
      cmocka_unit_test(test_vcard_get_foreign_domain_returns_not_found),
      cmocka_unit_test(test_vcard_update_existing_replaces_previous),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}