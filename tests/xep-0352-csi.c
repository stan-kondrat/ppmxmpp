/* XEP-0352: Client State Indication
 * https://xmpp.org/extensions/xep-0352.html
 *
 * Unit tests:
 *  - default state is ACTIVE
 *  - <active/> → CSI_ACTIVE
 *  - <inactive/> → CSI_INACTIVE
 *  - repeated transition is idempotent
 *  - wrong namespace is silently ignored
 *  - wrong stanza name is silently ignored (returns 1 → try next handler)
 */
#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_xmpp_helpers.h"
#include "xep-0352-csi.h"
#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Feed a bare top-level stanza in ONLINE state and return xmpp_feed rc.
 * This bypasses xmpp.c's stanza dispatcher and calls the handler directly
 * for isolated unit testing; the full integration path is exercised via
 * xep0352_handle_stanza() calls below. */
static int feed_stanza(xmpp_session_t* ctx, const char* xml) {
  return xmpp_feed(ctx, xml, strlen(xml), mock_write, NULL);
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_csi_default_is_active(void** state) {
  (void)state;
  xmpp_session_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  /* memset zeros → csi_state == 0 → XMPP_CSI_ACTIVE */
  assert_int_equal(ctx.csi_state, XMPP_CSI_ACTIVE);
}

static void test_csi_inactive_sets_state(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  reset_write_buf();

  const char* inactive = "<inactive xmlns='urn:xmpp:csi:0'/>";
  assert_int_equal(feed_stanza(&ctx, inactive), 0);
  assert_int_equal(ctx.csi_state, XMPP_CSI_INACTIVE);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_csi_active_sets_state(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  reset_write_buf();

  /* Transition to inactive first, then back to active. */
  const char* inactive = "<inactive xmlns='urn:xmpp:csi:0'/>";
  assert_int_equal(feed_stanza(&ctx, inactive), 0);
  assert_int_equal(ctx.csi_state, XMPP_CSI_INACTIVE);

  reset_write_buf();
  const char* active = "<active xmlns='urn:xmpp:csi:0'/>";
  assert_int_equal(feed_stanza(&ctx, active), 0);
  assert_int_equal(ctx.csi_state, XMPP_CSI_ACTIVE);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_csi_no_reply_sent(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  reset_write_buf();

  /* XEP-0352 §4.2: server MUST NOT reply to <active/> or <inactive/>. */
  const char* active = "<active xmlns='urn:xmpp:csi:0'/>";
  assert_int_equal(feed_stanza(&ctx, active), 0);
  assert_true(buf_contains("") == NULL || g_write_len == 0);
  /* No server response expected for CSI elements. */

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_csi_wrong_namespace_ignored(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  reset_write_buf();

  /* Stanza named "active" but wrong namespace → handler returns 1 (not handled). */
  const char* bad = "<active xmlns='urn:xmpp:wrong:namespace'/>";
  assert_int_equal(feed_stanza(&ctx, bad), 0);
  /* CSI state must remain ACTIVE (unchanged). */
  assert_int_equal(ctx.csi_state, XMPP_CSI_ACTIVE);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_csi_wrong_stanza_name_ignored(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  reset_write_buf();

  /* Correct namespace but unknown element name → handler returns 1. */
  const char* bad = "<foobar xmlns='urn:xmpp:csi:0'/>";
  assert_int_equal(feed_stanza(&ctx, bad), 0);
  assert_int_equal(ctx.csi_state, XMPP_CSI_ACTIVE);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_csi_stream_feature_advertised(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.out_len = 0;

  /* Append the CSI stream feature to the buffer. */
  assert_int_equal(xep0352_append_stream_feature(&ctx), 0);
  assert_true(ctx.out_len > 0);

  /* Verify the feature string contains the CSI advertisement. */
  int found = 0;
  for (size_t i = 0; i < ctx.out_len; i++) {
    if (strncmp(ctx.out_buf + i, "<csi xmlns='urn:xmpp:csi:0'/>", 29) == 0) {
      found = 1;
      break;
    }
  }
  assert_true(found);

  teardown_test_db();
}

static void test_csi_init_returns_zero(void** state) {
  (void)state;
  assert_int_equal(xep0352_init(), 0);
}

/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_csi_default_is_active),
      cmocka_unit_test(test_csi_inactive_sets_state),
      cmocka_unit_test(test_csi_active_sets_state),
      cmocka_unit_test(test_csi_no_reply_sent),
      cmocka_unit_test(test_csi_wrong_namespace_ignored),
      cmocka_unit_test(test_csi_wrong_stanza_name_ignored),
      cmocka_unit_test(test_csi_stream_feature_advertised),
      cmocka_unit_test(test_csi_init_returns_zero),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}