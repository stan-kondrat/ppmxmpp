/* XEP-0245: The /me Command — unit tests
 *
 * XEP-0245 is a purely client-side convention. The server:
 *   - routes /me messages unchanged (no body transformation)
 *   - advertises "urn:xmpp:me-command:0" in disco#info
 *
 * There is no server-side /me processing to test beyond these two behaviours.
 */
#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "test_xmpp_helpers.h"
#include "xmpp.h"
#include "xmpp_session.h"
#include "xep-0245-me-command.h"

/* ------------------------------------------------------------------ */
/*  R1: header constant                                                */
/* ------------------------------------------------------------------ */

static void test_me_command_ns_defined(void** state) {
  (void)state;
  /* The namespace macro must be defined to a non-empty string. */
  assert_non_null(XEP_0245_ME_COMMAND_NS);
  assert_string_not_equal(XEP_0245_ME_COMMAND_NS, "");
  assert_string_equal(XEP_0245_ME_COMMAND_NS, "urn:xmpp:me-command:0");

  /* Clean up so subsequent tests get a fresh database. */
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  R2: disco#info advertises the /me command feature                */
/* ------------------------------------------------------------------ */

static void test_disco_info_contains_me_command_feature(void** state) {
  (void)state;
  /* Clean up any stale state before setting up. */
  teardown_test_db();
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  reset_write_buf();

  const char* iq = "<iq id='m1' type='get' to='localhost'>"
                   "<query xmlns='http://jabber.org/protocol/disco#info'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);

  /* Verify the me-command feature is present. */
  assert_true(buf_contains("<feature var='urn:xmpp:me-command:0'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  R3: message body with /me prefix passes through unchanged        */
/* ------------------------------------------------------------------ */

/* Verify that the raw body text appears verbatim in the forwarded
 * message (no stripping, no transformation). This confirms the server
 * treats /me bodies as ordinary message content.
 *
 * Strategy: set up alice@localhost and testuser@localhost (both seeded
 * by setup_test_db). Register both sessions so they appear in the
 * session table. Feed a message from testuser to alice through
 * testuser's session. The routed message is written to alice's session
 * buffer (alice uses mock_write → g_write_buf), where the test checks it. */
static void test_me_message_body_unchanged(void** state) {
  (void)state;
  /* Clean up any stale state before setting up. */
  teardown_test_db();
  xmpp_session_table_reset_all();

  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Set up testuser@localhost. feed_to_online does memset + xmpp_session_reset
   * internally, so no need to reset beforehand. */
  xmpp_session_t testuser_ctx;
  assert_int_equal(feed_to_online(&testuser_ctx), 0);

  /* Set up alice@localhost. Must use feed_to_online_as so the session is
   * bound to alice@localhost (feed_to_online always authenticates as
   * testuser). */
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_to_online_as(&alice_ctx, "alice", "testpass"), 0);

  /* Register both sessions in the session table so message routing can
   * find the recipient. Without this, the recipient is treated as offline
   * and the message is stored rather than routed. */
  xmpp_session_table_register(&testuser_ctx, mock_write, NULL);
  xmpp_session_table_register(&alice_ctx, mock_write, NULL);

  reset_write_buf();

  /* testuser@localhost sends a /me message to alice@localhost.
   * The message handler writes the routed message to alice's session
   * buffer (alice's write_fn = mock_write → g_write_buf). The test feeds
   * through testuser's session to exercise the handler. */
  const char* msg = "<message from='testuser@localhost/res1' to='alice@localhost' id='m2'>"
                    "<body>/me waves at you</body></message>";
  assert_int_equal(xmpp_feed(&testuser_ctx, msg, strlen(msg), mock_write, NULL), 0);

  /* The body must appear verbatim (server does not modify it). */
  assert_true(buf_contains("<body>/me waves at you</body>"));

  /* The /me prefix must NOT have been stripped. */
  assert_false(buf_contains("<body>waves at you</body>"));
  assert_false(buf_contains("<body>* testuser waves at you</body>"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_cleanup(&testuser_ctx);
  xmpp_session_table_reset_all();
  teardown_test_db();
}

/* Non-/me messages also pass through unchanged (sanity check). */
static void test_regular_message_body_unchanged(void** state) {
  (void)state;
  /* Clean up any stale state before setting up. */
  teardown_test_db();
  xmpp_session_table_reset_all();

  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Set up testuser@localhost. feed_to_online does memset + xmpp_session_reset
   * internally, so no need to reset beforehand. */
  xmpp_session_t testuser_ctx;
  assert_int_equal(feed_to_online(&testuser_ctx), 0);

  /* Set up alice@localhost. Must use feed_to_online_as so the session is
   * bound to alice@localhost (feed_to_online always authenticates as
   * testuser). */
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_to_online_as(&alice_ctx, "alice", "testpass"), 0);

  /* Register both sessions for routing. */
  xmpp_session_table_register(&testuser_ctx, mock_write, NULL);
  xmpp_session_table_register(&alice_ctx, mock_write, NULL);

  reset_write_buf();

  /* Same routing as above: testuser sends to alice. */
  const char* msg = "<message from='testuser@localhost/res1' to='alice@localhost' id='m3'>"
                    "<body>hello world</body></message>";
  assert_int_equal(xmpp_feed(&testuser_ctx, msg, strlen(msg), mock_write, NULL), 0);

  assert_true(buf_contains("<body>hello world</body>"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_cleanup(&testuser_ctx);
  xmpp_session_table_reset_all();
  teardown_test_db();
}

/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_me_command_ns_defined),
      cmocka_unit_test(test_disco_info_contains_me_command_feature),
      cmocka_unit_test(test_me_message_body_unchanged),
      cmocka_unit_test(test_regular_message_body_unchanged),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}