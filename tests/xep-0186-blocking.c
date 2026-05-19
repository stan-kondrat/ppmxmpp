#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "storage/db_blocklist.h"
#include "storage/db_users.h"
#include "test_xmpp_helpers.h"
#include "xmpp.h"
#include "xmpp_session.h"

/* Per-test write sink (mirrors xep-0184-receipts.c). */
static void test_block_adds_jid_to_blocklist(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Block bob@localhost. */
  reset_write_buf();
  const char* iq = "<iq id='blk1' type='set'>"
                   "<query xmlns='jabber:iq:privacy'>"
                   "<block><item jid='bob@localhost'/></block>"
                   "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='blk1'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_block_multiple_jids(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  reset_write_buf();
  const char* iq = "<iq id='blk2' type='set'>"
                   "<query xmlns='jabber:iq:privacy'>"
                   "<block>"
                   "<item jid='bob@localhost'/>"
                   "<item jid='alice@example.com'/>"
                   "</block>"
                   "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='blk2'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_block_without_id(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  reset_write_buf();
  const char* iq = "<iq type='set'>"
                   "<query xmlns='jabber:iq:privacy'>"
                   "<block><item jid='bob@localhost'/></block>"
                   "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_block_empty_items_returns_error(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  reset_write_buf();
  const char* iq = "<iq id='blk3' type='set'>"
                   "<query xmlns='jabber:iq:privacy'>"
                   "<block/>"
                   "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("bad-request"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_unblock_removes_jid_from_blocklist(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* First block bob@localhost. */
  reset_write_buf();
  const char* block_iq = "<iq id='blk4' type='set'>"
                         "<query xmlns='jabber:iq:privacy'>"
                         "<block><item jid='bob@localhost'/></block>"
                         "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, block_iq, strlen(block_iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='blk4'/>"));

  /* Now unblock bob@localhost. */
  reset_write_buf();
  const char* unblock_iq = "<iq id='ublock1' type='set'>"
                            "<query xmlns='jabber:iq:privacy'>"
                            "<unblock><item jid='bob@localhost'/></unblock>"
                            "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, unblock_iq, strlen(unblock_iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='ublock1'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_unblock_empty_items_returns_error(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  reset_write_buf();
  const char* iq = "<iq id='ublock2' type='set'>"
                   "<query xmlns='jabber:iq:privacy'>"
                   "<unblock/>"
                   "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("bad-request"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_blocklist_get_empty(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  reset_write_buf();
  const char* iq = "<iq id='list1' type='get'>"
                   "<query xmlns='jabber:iq:privacy'/>"
                   "</iq>";
  assert_int_equal(xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='list1'>"));
  assert_true(buf_contains("<query xmlns='jabber:iq:privacy'>"));
  

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_blocklist_get_returns_blocked_jids(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Block two JIDs. */
  reset_write_buf();
  const char* block_iq = "<iq id='blk5' type='set'>"
                          "<query xmlns='jabber:iq:privacy'>"
                          "<block>"
                          "<item jid='bob@localhost'/>"
                          "<item jid='alice@localhost'/>"
                          "</block>"
                          "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, block_iq, strlen(block_iq), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='blk5'/>"));

  /* Retrieve blocklist. */
  reset_write_buf();
  const char* list_iq = "<iq id='list2' type='get'>"
                        "<query xmlns='jabber:iq:privacy'/>"
                        "</iq>";
  assert_int_equal(xmpp_feed(&ctx, list_iq, strlen(list_iq), mock_write, NULL), 0);
  
  assert_true(buf_contains("<iq type='result' id='list2'>"));
  assert_true(buf_contains("bob@localhost"));
  assert_true(buf_contains("alice@localhost"));
  assert_true(buf_contains("action='deny'"));
  assert_true(buf_contains("type='jid'"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

static void test_block_duplicate_is_idempotent(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Block the same JID twice. */
  reset_write_buf();
  const char* iq1 = "<iq id='blk6' type='set'>"
                    "<query xmlns='jabber:iq:privacy'>"
                    "<block><item jid='bob@localhost'/></block>"
                    "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq1, strlen(iq1), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='blk6'/>"));

  reset_write_buf();
  const char* iq2 = "<iq id='blk7' type='set'>"
                    "<query xmlns='jabber:iq:privacy'>"
                    "<block><item jid='bob@localhost'/></block>"
                    "</query></iq>";
  assert_int_equal(xmpp_feed(&ctx, iq2, strlen(iq2), mock_write, NULL), 0);
  assert_true(buf_contains("<iq type='result' id='blk7'/>"));

  /* Blocklist should still show only one entry. */
  reset_write_buf();
  const char* list_iq = "<iq id='list3' type='get'>"
                         "<query xmlns='jabber:iq:privacy'/>"
                         "</iq>";
  assert_int_equal(xmpp_feed(&ctx, list_iq, strlen(list_iq), mock_write, NULL), 0);
  
  assert_true(buf_contains("<iq type='result' id='list3'>"));
  /* Count occurrences of bob@localhost — should appear exactly once. */
  const char* pos = buf_contains("bob@localhost");
  assert_non_null(pos);
  const char* second = pos + 1;
  const char* found = memmem(second, (size_t)g_write_len - (size_t)(second - g_write_buf), "bob@localhost", 14);
  assert_null(found); /* Should not appear twice. */

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_block_adds_jid_to_blocklist),
      cmocka_unit_test(test_block_multiple_jids),
      cmocka_unit_test(test_block_without_id),
      cmocka_unit_test(test_block_empty_items_returns_error),
      cmocka_unit_test(test_unblock_removes_jid_from_blocklist),
      cmocka_unit_test(test_unblock_empty_items_returns_error),
      cmocka_unit_test(test_blocklist_get_empty),
      cmocka_unit_test(test_blocklist_get_returns_blocked_jids),
      cmocka_unit_test(test_block_duplicate_is_idempotent),
  };
  return cmocka_run_group_tests(tests, log_group_setup, log_group_teardown);
}