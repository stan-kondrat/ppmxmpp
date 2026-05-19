/* XEP-0198: Stream Management — unit tests
 * https://xmpp.org/extensions/xep-0198.html
 *
 * Tests cover:
 *   1. <enable/> triggers <enabled/> without resume.
 *   2. <enable resume='true'/> triggers <enabled resume='true' id='...'>.
 *   3. Duplicate <enable/> causes a stream error.
 *   4. <r/> in ONLINE state triggers <a h='0'/> ack response.
 *   5. <a h='N'/> with valid N updates sm_outbound.
 *   6. <a h='N'/> with h > sent stanzas triggers handled-count-too-high error.
 *   7. <resume/> with invalid SM-ID triggers <failed/> with item-not-found.
 */
#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_xmpp_helpers.h"
#include "xep-0198-stream-mgmt.h"
#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Helper: feed ctx to ONLINE state (wraps feed_to_online).           */
/* ------------------------------------------------------------------ */

static int feed_to_sm_online(xmpp_session_t* ctx) {
  g_write_len = 0;
  return feed_to_online(ctx);
}

/* ------------------------------------------------------------------ */
/*  Test: <enable/> → <enabled/> (no resume)                           */
/* ------------------------------------------------------------------ */

static void test_enable_no_resume(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Client sends <enable/> — SM session begins. */
  const char* enable = "<enable xmlns='urn:xmpp:sm:3'/>";
  assert_int_equal(xmpp_feed(&ctx, enable, strlen(enable), mock_write, NULL), 0);

  /* Server must have sent <enabled xmlns='urn:xmpp:sm:3'/>. */
  assert_non_null(buf_contains("<enabled xmlns='urn:xmpp:sm:3'/>"));
  /* No resume attribute since client didn't request it. */
  assert_null(buf_contains("resume='true'"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <enable resume='true'/> → <enabled id='...' resume='true'/>  */
/* ------------------------------------------------------------------ */

static void test_enable_with_resume(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Client requests session resumption. */
  const char* enable_resume = "<enable xmlns='urn:xmpp:sm:3' resume='true'/>";
  assert_int_equal(xmpp_feed(&ctx, enable_resume, strlen(enable_resume), mock_write, NULL), 0);

  /* Server must have sent <enabled resume='true' id='...'>.
   * The id is HMAC-SHA256 hex (64 hex chars = 32 bytes). */
  assert_non_null(buf_contains("<enabled xmlns='urn:xmpp:sm:3' resume='true'"));
  assert_non_null(buf_contains("id='"));
  assert_non_null(buf_contains("max='"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: duplicate <enable/> → stream error                           */
/* ------------------------------------------------------------------ */

static void test_enable_duplicate(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* First <enable/>. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<enable xmlns='urn:xmpp:sm:3'/>",
                             strlen("<enable xmlns='urn:xmpp:sm:3'/>"),
                             mock_write, NULL), 0);
  assert_non_null(buf_contains("<enabled xmlns='urn:xmpp:sm:3'/>"));

  /* Second <enable/> — server must return stream error. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<enable xmlns='urn:xmpp:sm:3'/>",
                             strlen("<enable xmlns='urn:xmpp:sm:3'/>"),
                             mock_write, NULL), 0);
  /* Stream error with unexpected-request. */
  assert_non_null(buf_contains("<stream:error>"));
  assert_non_null(buf_contains("<unexpected-request"));
  assert_non_null(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <r/> → <a h='0'/>                                            */
/* ------------------------------------------------------------------ */

static void test_ack_request(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Enable SM first. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<enable xmlns='urn:xmpp:sm:3'/>",
                             strlen("<enable xmlns='urn:xmpp:sm:3'/>"),
                             mock_write, NULL), 0);
  assert_non_null(buf_contains("<enabled xmlns='urn:xmpp:sm:3'/>"));

  /* Client requests acknowledgement. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<r xmlns='urn:xmpp:sm:3'/>",
                             strlen("<r xmlns='urn:xmpp:sm:3'/>"),
                             mock_write, NULL), 0);

  /* Server responds with <a h='0'/> (zero stanzas handled so far). */
  assert_non_null(buf_contains("<a xmlns='urn:xmpp:sm:3' h='0'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <a h='N'/> updates sm_outbound counter                        */
/* ------------------------------------------------------------------ */

static void test_ack_response_valid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Enable SM. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<enable xmlns='urn:xmpp:sm:3'/>",
                             strlen("<enable xmlns='urn:xmpp:sm:3'/>"),
                             mock_write, NULL), 0);

  /* Send two stanzas (IQ and message) to advance sm_handled on server. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx,
                              "<iq type='get' id='x1'><ping xmlns='urn:xmpp:ping'/></iq>"
                              "<message to='bob@localhost'><body>hi</body></message>",
                              strlen("<iq type='get' id='x1'><ping xmlns='urn:xmpp:ping'/></iq>"
                                    "<message to='bob@localhost'><body>hi</body></message>"),
                              mock_write, NULL), 0);

  /* Client acks receipt of 2 stanzas. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<a xmlns='urn:xmpp:sm:3' h='2'/>",
                             strlen("<a xmlns='urn:xmpp:sm:3' h='2'/>"),
                             mock_write, NULL), 0);

  /* No error response — ack was valid. */
  assert_null(buf_contains("<stream:error>"));
  assert_int_equal(ctx.sm_outbound, 2U);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <a h='N'/> with h > sent stanzas → handled-count-too-high    */
/* ------------------------------------------------------------------ */

static void test_ack_response_too_high(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Enable SM — no stanzas sent yet (sm_outbound == 0). */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<enable xmlns='urn:xmpp:sm:3'/>",
                             strlen("<enable xmlns='urn:xmpp:sm:3'/>"),
                             mock_write, NULL), 0);

  /* Client claims to have handled 5 stanzas but server sent 0. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<a xmlns='urn:xmpp:sm:3' h='5'/>",
                             strlen("<a xmlns='urn:xmpp:sm:3' h='5'/>"),
                             mock_write, NULL), 0);

  /* Server must return handled-count-too-high stream error. */
  assert_non_null(buf_contains("<stream:error>"));
  assert_non_null(buf_contains("<handled-count-too-high"));
  assert_non_null(buf_contains("</stream:stream>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <resume/> with unknown SM-ID → <failed/>                     */
/* ------------------------------------------------------------------ */

static void test_resume_bad_smid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Client attempts resumption with a fabricated SM-ID. */
  reset_write_buf();
  const char* bad_resume =
      "<resume xmlns='urn:xmpp:sm:3' h='0' previd='this-is-a-fake-sm-id'/>";
  assert_int_equal(xmpp_feed(&ctx, bad_resume, strlen(bad_resume), mock_write, NULL), 0);

  /* Server must send <failed/> with item-not-found. */
  assert_non_null(buf_contains("<failed xmlns='urn:xmpp:sm:3'>"));
  assert_non_null(buf_contains("<item-not-found"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <resume/> with valid SM-ID (after enable+resume)            */
/* ------------------------------------------------------------------ */

static void test_resume_valid_smid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Enable SM with resumption to get a valid SM-ID. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<enable xmlns='urn:xmpp:sm:3' resume='true'/>",
                             strlen("<enable xmlns='urn:xmpp:sm:3' resume='true'/>"),
                             mock_write, NULL), 0);

  /* Extract the SM-ID from the <enabled/> element. */
  const char* enabled_ptr = buf_contains("id='");
  assert_non_null(enabled_ptr);
  const char* id_start = enabled_ptr + 4;  /* skip "id='" */
  const char* id_end = strchr(id_start, '\'');
  assert_non_null(id_end);

  char sm_id[65];
  size_t id_len = (size_t)(id_end - id_start);
  if (id_len >= sizeof(sm_id)) id_len = sizeof(sm_id) - 1;
  memcpy(sm_id, id_start, id_len);
  sm_id[id_len] = '\0';
  assert_true(id_len > 0);

  /* Verify ctx.sm_id was stored. */
  assert_string_equal(sm_id, ctx.sm_id);

  /* Client sends <resume/> with the valid SM-ID.
   * Since the session is the same (no disconnect), the server should
   * still accept it (in a real scenario, the server would look up the
   * SM-ID in its session table; here we test the string comparison). */
  reset_write_buf();
  char resume_xml[256];
  snprintf(resume_xml, sizeof(resume_xml),
           "<resume xmlns='urn:xmpp:sm:3' h='0' previd='%s'/>", sm_id);
  assert_int_equal(xmpp_feed(&ctx, resume_xml, strlen(resume_xml), mock_write, NULL), 0);

  /* Server must send <resumed/>. */
  assert_non_null(buf_contains("<resumed xmlns='urn:xmpp:sm:3'"));
  assert_non_null(buf_contains("previd='"));
  assert_non_null(buf_contains(" h='"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: SM element before <enable/> is ignored (pre-XEP-0198 client)  */
/* ------------------------------------------------------------------ */

static void test_r_before_enable_ignored(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_sm_online(&ctx), 0);

  /* Client sends <r/> without prior <enable/> — should be ignored. */
  reset_write_buf();
  assert_int_equal(xmpp_feed(&ctx, "<r xmlns='urn:xmpp:sm:3'/>",
                             strlen("<r xmlns='urn:xmpp:sm:3'/>"),
                             mock_write, NULL), 0);

  /* No <a/> response since SM was not enabled. */
  assert_null(buf_contains("<a xmlns='urn:xmpp:sm:3'"));
  /* Session should remain ONLINE (not errored). */
  assert_int_equal(ctx.state, XMPP_STATE_ONLINE);
  assert_int_equal(ctx.pending_error, 0);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: SM feature is advertised in stream features post-SASL         */
/* ------------------------------------------------------------------ */

static void test_sm_feature_in_stream_features(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream → receive features. */
  char buf[512];
  snprintf(buf, sizeof(buf),
           "<?xml version='1.0'?>"
           "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
           " xmlns='jabber:client' to='localhost' version='1.0'>");
  assert_int_equal(xmpp_feed(&ctx, buf, strlen(buf), mock_write, NULL), 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  g_write_len = 0;
  (void)snprintf(ctx.domain, sizeof(ctx.domain), "%s", "localhost");
  assert_int_equal(xmpp_feed(&ctx, "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>",
                             strlen("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>"),
                             mock_write, NULL), 0);

  g_write_len = 0;
  const char* r1 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  assert_int_equal(xmpp_feed(&ctx, r1, strlen(r1), mock_write, NULL), 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  if (feed_sasl_plain(&ctx, "", "testuser", "testpass") == 0) {
    assert_int_equal(ctx.state, XMPP_STATE_SASL_SUCCESS);

    g_write_len = 0;
    const char* r2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                     " xmlns='jabber:client' to='localhost' version='1.0'>";
    assert_int_equal(xmpp_feed(&ctx, r2, strlen(r2), mock_write, NULL), 0);

    /* After SASL + stream restart, server offers SM in stream features. */
    assert_non_null(buf_contains("<sm xmlns='urn:xmpp:sm:3'/>"));
  }

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test suite                                                         */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_enable_no_resume),
    cmocka_unit_test(test_enable_with_resume),
    cmocka_unit_test(test_enable_duplicate),
    cmocka_unit_test(test_ack_request),
    cmocka_unit_test(test_ack_response_valid),
    cmocka_unit_test(test_ack_response_too_high),
    cmocka_unit_test(test_resume_bad_smid),
    cmocka_unit_test(test_resume_valid_smid),
    cmocka_unit_test(test_r_before_enable_ignored),
    cmocka_unit_test(test_sm_feature_in_stream_features),
  };
  return cmocka_run_group_tests(tests, log_group_setup, log_group_teardown);
}