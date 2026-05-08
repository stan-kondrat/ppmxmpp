#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_xmpp_helpers.h"
#include "xmpp.h"


static void test_ping_returns_result(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='p1' type='get'><ping xmlns='urn:xmpp:ping'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='p1'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_ping_without_id(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq type='get'><ping xmlns='urn:xmpp:ping'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_ping_set_returns_feature_not_implemented(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  const char* iq = "<iq id='p2' type='set'><ping xmlns='urn:xmpp:ping'/></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<feature-not-implemented"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_ping_returns_result),
      cmocka_unit_test(test_ping_without_id),
      cmocka_unit_test(test_ping_set_returns_feature_not_implemented),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
