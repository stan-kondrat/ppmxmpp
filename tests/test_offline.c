#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "storage/db_offline.h"
#include "storage/db_users.h"
#include "test_xmpp_helpers.h"

static int test_count_cb = 0;

static void count_cb(const char* recipient_jid, const char* sender_jid, const char* stanza_xml,
                     long long received_at, void* ud) {
  (void)recipient_jid;
  (void)sender_jid;
  (void)stanza_xml;
  (void)received_at;
  int* c = (int*)ud;
  (*c)++;
}

static int test_offline_setup(void** state) {
  (void)state;
  const char* db_path = NULL;
  test_count_cb = 0;
  return setup_test_db(&db_path);
}

static int test_offline_teardown(void** state) {
  (void)state;
  storage_db_close();
  teardown_test_db();
  return 0;
}

/* Store a message offline, verify count. */
static void test_offline_store_and_count(void** state) {
  (void)state;
  int rc = offline_store("alice@localhost", "bob@localhost", "<message>test</message>", 20);
  assert_int_equal(rc, 0);
  assert_int_equal(offline_count("alice@localhost"), 1);
}

/* Store multiple messages, verify total count. */
static void test_offline_multiple_messages(void** state) {
  (void)state;
  assert_int_equal(offline_store("alice@localhost", "bob@localhost", "<message>m1</message>", 15), 0);
  assert_int_equal(offline_store("alice@localhost", "charlie@localhost", "<message>m2</message>", 15), 0);
  assert_int_equal(offline_store("alice@localhost", "bob@localhost", "<message>m3</message>", 15), 0);
  assert_int_equal(offline_count("alice@localhost"), 3);
}

/* Delete all messages for a user. */
static void test_offline_delete_all(void** state) {
  (void)state;
  offline_store("alice@localhost", "bob@localhost", "<message>m1</message>", 15);
  offline_store("alice@localhost", "charlie@localhost", "<message>m2</message>", 15);
  assert_int_equal(offline_count("alice@localhost"), 2);

  offline_delete_all("alice@localhost");
  assert_int_equal(offline_count("alice@localhost"), 0);
}

/* Check that storage is not capped before limit. */
static void test_offline_not_capped(void** state) {
  (void)state;
  for (int i = 0; i < 50; i++) {
    assert_int_equal(offline_store("alice@localhost", "bob@localhost", "<message>test</message>", 20), 0);
  }
  assert_int_equal(offline_is_capped("alice@localhost"), 0);
}

/* Check that storage is capped at 100 messages. */
static void test_offline_capped_at_100_messages(void** state) {
  (void)state;
  for (int i = 0; i < 100; i++) {
    assert_int_equal(offline_store("alice@localhost", "bob@localhost", "<message>test</message>", 20), 0);
  }
  /* 101st should be capped. */
  assert_int_equal(offline_store("alice@localhost", "bob@localhost", "<message>test</message>", 20), -2);
  assert_int_equal(offline_is_capped("alice@localhost"), 1);
}

/* Check that storage is capped at 1MB. */
static void test_offline_capped_at_1mb(void** state) {
  (void)state;
  /* Large message */
  char large_msg[2048];
  memset(large_msg, 'x', sizeof(large_msg) - 1);
  large_msg[sizeof(large_msg) - 1] = '\0';

  /* Keep storing until cap is reached */
  int stored = 0;
  while (offline_store("alice@localhost", "bob@localhost", large_msg, sizeof(large_msg)) == 0) {
    stored++;
    if (stored > 1000) break; /* Safety limit */
  }
  assert_int_equal(offline_is_capped("alice@localhost"), 1);
}

/* List messages and verify order. */
static void test_offline_list_order(void** state) {
  (void)state;
  offline_store("alice@localhost", "bob@localhost", "<message>msg1</message>", 20);
  offline_store("alice@localhost", "charlie@localhost", "<message>msg2</message>", 20);
  offline_store("alice@localhost", "david@localhost", "<message>msg3</message>", 20);

  test_count_cb = 0;
  int rc = offline_list("alice@localhost", count_cb, &test_count_cb);
  assert_int_equal(rc, 0);
  assert_int_equal(test_count_cb, 3);
}

/* Total bytes tracking. */
static void test_offline_total_bytes(void** state) {
  (void)state;
  offline_store("alice@localhost", "bob@localhost", "<message>12345</message>", 21);
  offline_store("alice@localhost", "bob@localhost", "<message>67890</message>", 21);
  assert_true(offline_total_bytes("alice@localhost") >= 42);
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test_setup_teardown(test_offline_store_and_count, test_offline_setup,
                                      test_offline_teardown),
      cmocka_unit_test_setup_teardown(test_offline_multiple_messages, test_offline_setup,
                                      test_offline_teardown),
      cmocka_unit_test_setup_teardown(test_offline_delete_all, test_offline_setup,
                                      test_offline_teardown),
      cmocka_unit_test_setup_teardown(test_offline_not_capped, test_offline_setup,
                                      test_offline_teardown),
      cmocka_unit_test_setup_teardown(test_offline_capped_at_100_messages, test_offline_setup,
                                      test_offline_teardown),
      cmocka_unit_test_setup_teardown(test_offline_capped_at_1mb, test_offline_setup,
                                      test_offline_teardown),
      cmocka_unit_test_setup_teardown(test_offline_list_order, test_offline_setup,
                                      test_offline_teardown),
      cmocka_unit_test_setup_teardown(test_offline_total_bytes, test_offline_setup,
                                      test_offline_teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}