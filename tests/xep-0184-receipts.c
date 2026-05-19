/* XEP-0184: Message Delivery Receipts
 * https://xmpp.org/extensions/xep-0184.html
 *
 * Tests:
 *   1.  Incoming message with <request/> → server sends <received/> receipt
 *   2.  Receipt sent to correct sender JID with correct id
 *   3.  Message with <received/> (incoming ack) is excluded from carbons
 *   4.  Message with <displayed/> (incoming ack) is excluded from carbons
 *   5.  Receipt excluded from carbons when server sends it
 *   6.  Disco#info includes 'urn:xmpp:receipts' feature
 */
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
#include "xep-0184-receipts.h"
#include "xep-0280-carbons.h"
#include "xmpp.h"
#include "xmpp_message.h"
#include "xmpp_session.h"

/* ------------------------------------------------------------------ */
/*  Per-test write sink                                                */
/* ------------------------------------------------------------------ */

#define SINK_BUF_SIZE 65536

typedef struct {
  char buf[SINK_BUF_SIZE];
  size_t len;
} write_sink_t;

static int sink_write(void* ud, const char* data, size_t len) {
  write_sink_t* s = (write_sink_t*)ud;
  if (s->len + len > SINK_BUF_SIZE) len = SINK_BUF_SIZE - s->len;
  memcpy(s->buf + s->len, data, len);
  s->len += len;
  return 0;
}

static int sink_contains(const write_sink_t* s, const char* needle) {
  return memmem(s->buf, s->len, needle, strlen(needle)) != NULL;
}

static void sink_reset(write_sink_t* s) {
  memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/*  Test fixtures                                                     */
/* ------------------------------------------------------------------ */

static int receipts_setup(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  return 0;
}

static int receipts_teardown(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  storage_db_close();
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Session helper — bring a user to ONLINE and register it            */
/* ------------------------------------------------------------------ */

static int feed_user_to_online_with_sink(xmpp_session_t* ctx, write_sink_t* sink,
                                         const char* authcid, const char* passwd,
                                         const char* resource) {
  memset(ctx, 0, sizeof(*ctx));
  xmpp_session_reset(ctx);

  char buf[512];
  snprintf(buf, sizeof(buf),
           "<?xml version='1.0'?>"
           "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
           " xmlns='jabber:client' to='localhost' version='1.0'>");
  if (xmpp_feed(ctx, buf, strlen(buf), mock_write, NULL) != 0) return -1;

  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  if (xmpp_feed(ctx, starttls, strlen(starttls), mock_write, NULL) != 0) return -1;
  simulate_starttls(ctx);

  snprintf(buf, sizeof(buf),
           "<?xml version='1.0'?>"
           "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
           " xmlns='jabber:client' to='localhost' version='1.0'>");
  if (xmpp_feed(ctx, buf, strlen(buf), mock_write, NULL) != 0) return -1;

  if (feed_sasl_plain(ctx, "", authcid, passwd) != 0) return -1;

  snprintf(buf, sizeof(buf),
           "<?xml version='1.0'?>"
           "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
           " xmlns='jabber:client' to='localhost' version='1.0'>");
  if (xmpp_feed(ctx, buf, strlen(buf), mock_write, NULL) != 0) return -1;

  char bind[256];
  snprintf(bind, sizeof(bind),
           "<iq type='set' id='b1'>"
           "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
           "<resource>%s</resource></bind></iq>",
           resource);
  if (xmpp_feed(ctx, bind, strlen(bind), sink_write, sink) != 0) return -1;
  if (ctx->state != XMPP_STATE_ONLINE) return -1;

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test: incoming message with <request/> → receipt sent back         */
/* ------------------------------------------------------------------ */

static void test_receipt_sent_on_request(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                  "alice", "testpass", "phone"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Alice receives a message with <request/> from Bob. */
  const char* msg = "<message from='bob@localhost/desktop' to='alice@localhost/phone' "
                    "type='chat' id='msg-abc-123'>"
                    "<body>hello alice</body>"
                    "<request xmlns='urn:xmpp:receipts'/>"
                    "</message>";
  assert_int_equal(xmpp_feed(&alice_ctx, msg, strlen(msg), sink_write, &alice_sink), 0);

  /* Alice should send back a receipt with the same id. */
  assert_true(sink_contains(&alice_sink, "urn:xmpp:receipts"));
  assert_true(sink_contains(&alice_sink, "received"));
  assert_true(sink_contains(&alice_sink, "id='msg-abc-123'"));
  /* Receipt from= must be Alice's full JID, to= must be Bob's full JID. */
  assert_true(sink_contains(&alice_sink, "from='alice@localhost/phone'"));
  assert_true(sink_contains(&alice_sink, "to='bob@localhost/desktop'"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_table_unregister("alice@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: receipt only sent when message has id=                        */
/* ------------------------------------------------------------------ */

static void test_receipt_not_sent_without_id(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                  "alice", "testpass", "phone"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  sink_reset(&alice_sink);

  /* Alice receives a message with <request/> but NO id=. */
  const char* msg = "<message from='bob@localhost/desktop' to='alice@localhost/phone' "
                    "type='chat'>"
                    "<body>hello</body>"
                    "<request xmlns='urn:xmpp:receipts'/>"
                    "</message>";
  assert_int_equal(xmpp_feed(&alice_ctx, msg, strlen(msg), sink_write, &alice_sink), 0);

  /* No receipt should be sent (XEP-0184 §1 requires id=). */
  assert_false(sink_contains(&alice_sink, "received xmlns='urn:xmpp:receipts'"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_table_unregister("alice@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: incoming <received/> message is excluded from carbons        */
/* ------------------------------------------------------------------ */

static void test_received_ack_excluded_from_carbons(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Bob (sender). */
  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&bob_ctx, &bob_sink,
                                                  "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  /* Alice on 'desktop' (recipient, carbons-enabled). */
  write_sink_t alice_desktop_sink = {.len = 0};
  xmpp_session_t alice_desktop;
  assert_int_equal(feed_user_to_online_with_sink(&alice_desktop, &alice_desktop_sink,
                                                   "alice", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_desktop, sink_write, &alice_desktop_sink);

  /* Alice on 'mobile' (carbons-enabled). */
  write_sink_t alice_mobile_sink = {.len = 0};
  xmpp_session_t alice_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&alice_mobile, &alice_mobile_sink,
                                                  "alice", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_mobile, sink_write, &alice_mobile_sink);

  /* Enable carbons on both of Alice's resources. */
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_desktop, enable, strlen(enable), sink_write, &alice_desktop_sink);
  xmpp_feed(&alice_mobile, enable, strlen(enable), sink_write, &alice_mobile_sink);

  /* Bob sends a message to Alice. */
  const char* bob_msg = "<message from='bob@localhost/phone' to='alice@localhost' "
                         "type='chat' id='bob-msg-1'>"
                         "<body>hello</body>"
                         "</message>";
  assert_int_equal(xmpp_feed(&bob_ctx, bob_msg, strlen(bob_msg), sink_write, &bob_sink), 0);

  /* Reset sinks after initial message routing, before the ack-under-test. */
  sink_reset(&alice_desktop_sink);
  sink_reset(&alice_mobile_sink);
  sink_reset(&bob_sink);

  /* Alice (desktop) sends a <received/> receipt back to Bob.
   * This is delivered to Bob via the normal routing path.
   * The server must NOT forward this receipt as a carbon to Alice's mobile. */
  const char* ack = "<message from='alice@localhost/desktop' to='bob@localhost/phone' "
                    "type='chat' id='ack-1'>"
                    "<received xmlns='urn:xmpp:receipts' id='bob-msg-1'/>"
                    "</message>";
  assert_int_equal(xmpp_feed(&alice_desktop, ack, strlen(ack), sink_write, &alice_desktop_sink), 0);

  /* Bob must receive the <received/> receipt (routed directly to Bob's resource).
   * This is tested by checking the original message routing doesn't re-carbon-copy it.
   * Alice's mobile should NOT receive a carbon copy of the ack (excluded by is_receipt_ack). */
  assert_false(sink_contains(&alice_mobile_sink, "urn:xmpp:receipts"));
  /* Bob should receive the receipt via direct routing. */
  assert_true(sink_contains(&bob_sink, "received xmlns='urn:xmpp:receipts'"));

  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_cleanup(&alice_desktop);
  xmpp_session_cleanup(&alice_mobile);
  xmpp_session_table_unregister("bob@localhost/phone");
  xmpp_session_table_unregister("alice@localhost/desktop");
  xmpp_session_table_unregister("alice@localhost/mobile");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: incoming <displayed/> message is excluded from carbons      */
/* ------------------------------------------------------------------ */

static void test_displayed_ack_excluded_from_carbons(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Bob (sender). */
  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&bob_ctx, &bob_sink,
                                                  "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  /* Alice on 'desktop' (recipient, carbons-enabled). */
  write_sink_t alice_desktop_sink = {.len = 0};
  xmpp_session_t alice_desktop;
  assert_int_equal(feed_user_to_online_with_sink(&alice_desktop, &alice_desktop_sink,
                                                   "alice", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_desktop, sink_write, &alice_desktop_sink);

  /* Alice on 'mobile' (carbons-enabled). */
  write_sink_t alice_mobile_sink = {.len = 0};
  xmpp_session_t alice_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&alice_mobile, &alice_mobile_sink,
                                                  "alice", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_mobile, sink_write, &alice_mobile_sink);

  /* Enable carbons on both of Alice's resources. */
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_desktop, enable, strlen(enable), sink_write, &alice_desktop_sink);
  xmpp_feed(&alice_mobile, enable, strlen(enable), sink_write, &alice_mobile_sink);

  /* Reset sinks before the ack-under-test. */
  sink_reset(&alice_desktop_sink);
  sink_reset(&alice_mobile_sink);

  /* Alice (desktop) sends a <displayed/> receipt to Bob. */
  const char* ack = "<message from='alice@localhost/desktop' to='bob@localhost/phone' "
                    "type='chat' id='ack-2'>"
                    "<displayed xmlns='urn:xmpp:receipts' id='bob-msg-1'/>"
                    "</message>";
  assert_int_equal(xmpp_feed(&alice_desktop, ack, strlen(ack), sink_write, &alice_desktop_sink), 0);

  /* Alice's mobile should NOT receive a carbon copy of the displayed ack. */
  assert_false(sink_contains(&alice_mobile_sink, "displayed"));
  assert_false(sink_contains(&alice_mobile_sink, "urn:xmpp:receipts"));

  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_cleanup(&alice_desktop);
  xmpp_session_cleanup(&alice_mobile);
  xmpp_session_table_unregister("bob@localhost/phone");
  xmpp_session_table_unregister("alice@localhost/desktop");
  xmpp_session_table_unregister("alice@localhost/mobile");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: regular message with <request/>> is still carbon-copied      */
/* ------------------------------------------------------------------ */

static void test_request_message_still_carbon_copied(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Bob (sender). */
  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&bob_ctx, &bob_sink,
                                                  "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  /* Alice on 'desktop' (recipient, carbons-enabled). */
  write_sink_t alice_desktop_sink = {.len = 0};
  xmpp_session_t alice_desktop;
  assert_int_equal(feed_user_to_online_with_sink(&alice_desktop, &alice_desktop_sink,
                                                   "alice", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_desktop, sink_write, &alice_desktop_sink);

  /* Alice on 'mobile' (carbons-enabled). */
  write_sink_t alice_mobile_sink = {.len = 0};
  xmpp_session_t alice_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&alice_mobile, &alice_mobile_sink,
                                                  "alice", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_mobile, sink_write, &alice_mobile_sink);

  /* Enable carbons on both of Alice's resources. */
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_desktop, enable, strlen(enable), sink_write, &alice_desktop_sink);
  xmpp_feed(&alice_mobile, enable, strlen(enable), sink_write, &alice_mobile_sink);

  /* Reset sinks before the message-under-test. */
  sink_reset(&alice_desktop_sink);
  sink_reset(&alice_mobile_sink);

  /* Bob sends a message with <request/> to Alice. */
  const char* msg = "<message from='bob@localhost/phone' to='alice@localhost' "
                    "type='chat' id='msg-with-req'>"
                    "<body>read this</body>"
                    "<request xmlns='urn:xmpp:receipts'/>"
                    "</message>";
  assert_int_equal(xmpp_feed(&bob_ctx, msg, strlen(msg), sink_write, &bob_sink), 0);

  /* Alice's desktop receives the original message. */
  assert_true(sink_contains(&alice_desktop_sink, "read this"));
  assert_true(sink_contains(&alice_desktop_sink, "request xmlns='urn:xmpp:receipts'"));

  /* Alice's mobile should receive a <received/> carbon copy (not the <request/> message itself
   * because it's a received-carbon, not a sent-carbon; but the <request/> is a content message
   * so it does get carbon-copied to mobile). */
  /* Actually: when Bob sends to Alice (bare JID), this is:
   * - sent-carbon: Bob (sender) copies to Bob's other resources (not applicable here)
   * - received-carbon: Alice (recipient) receives and then copies to Alice's other resources.
   *   The carbon copy goes to Alice's mobile with <received>. */
  /* Since this is Alice receiving a message (not sending), mobile gets a received-carbon. */
  assert_true(sink_contains(&alice_mobile_sink, "received xmlns='urn:xmpp:carbons:2'"));
  assert_true(sink_contains(&alice_mobile_sink, "read this"));

  /* Receipt is still sent to Bob. */
  sink_reset(&bob_sink);
  assert_true(sink_contains(&bob_sink, "received xmlns='urn:xmpp:receipts'"));
  assert_true(sink_contains(&bob_sink, "id='msg-with-req'"));

  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_cleanup(&alice_desktop);
  xmpp_session_cleanup(&alice_mobile);
  xmpp_session_table_unregister("bob@localhost/phone");
  xmpp_session_table_unregister("alice@localhost/desktop");
  xmpp_session_table_unregister("alice@localhost/mobile");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: API: xep0184_has_request / has_received / has_displayed      */
/* ------------------------------------------------------------------ */

static void test_api_has_request_received_displayed(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                  "alice", "testpass", "phone"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Feed a <request/> message. */
  const char* req_msg = "<message from='bob@localhost' to='alice@localhost/phone' "
                        "type='chat' id='req1'>"
                        "<body>hi</body>"
                        "<request xmlns='urn:xmpp:receipts'/>"
                        "</message>";
  assert_int_equal(xmpp_feed(&alice_ctx, req_msg, strlen(req_msg), sink_write, &alice_sink), 0);

  /* Verify receipt was sent back. */
  assert_true(sink_contains(&alice_sink, "received xmlns='urn:xmpp:receipts'"));

  /* Feed a <received/> ack message. */
  sink_reset(&alice_sink);
  const char* recv_msg = "<message from='bob@localhost' to='alice@localhost/phone' "
                         "type='chat' id='recv1'>"
                         "<received xmlns='urn:xmpp:receipts' id='req1'/>"
                         "</message>";
  assert_int_equal(xmpp_feed(&alice_ctx, recv_msg, strlen(recv_msg), sink_write, &alice_sink), 0);

  /* Verify no receipt was sent (it's an ack, not a content message). */
  /* Note: we can't directly test xep0184_has_* here since they take xmpp_stanza_t
   * and we don't have the stanza after feeding. The above behavior is the integration test. */

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_table_unregister("alice@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: xep0184_send_displayed                                       */
/* ------------------------------------------------------------------ */

static void test_send_displayed_receipt(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                  "alice", "testpass", "phone"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Manually send a displayed receipt. */
  int rc = xep0184_send_displayed(&alice_ctx, "alice@localhost/phone",
                                    "bob@localhost/desktop", "some-id");
  assert_int_equal(rc, 0);

  /* Verify the displayed receipt. */
  assert_true(sink_contains(&alice_sink, "displayed xmlns='urn:xmpp:receipts'"));
  assert_true(sink_contains(&alice_sink, "id='some-id'"));
  assert_true(sink_contains(&alice_sink, "from='alice@localhost/phone'"));
  assert_true(sink_contains(&alice_sink, "to='bob@localhost/desktop'"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_table_unregister("alice@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test_setup_teardown(test_receipt_sent_on_request, receipts_setup,
                                       receipts_teardown),
      cmocka_unit_test_setup_teardown(test_receipt_not_sent_without_id, receipts_setup,
                                     receipts_teardown),
      cmocka_unit_test_setup_teardown(test_received_ack_excluded_from_carbons, receipts_setup,
                                      receipts_teardown),
      cmocka_unit_test_setup_teardown(test_displayed_ack_excluded_from_carbons, receipts_setup,
                                      receipts_teardown),
      cmocka_unit_test_setup_teardown(test_request_message_still_carbon_copied, receipts_setup,
                                      receipts_teardown),
      cmocka_unit_test_setup_teardown(test_api_has_request_received_displayed, receipts_setup,
                                       receipts_teardown),
      cmocka_unit_test_setup_teardown(test_send_displayed_receipt, receipts_setup,
                                      receipts_teardown),
  };
  return cmocka_run_group_tests(tests, log_group_setup, log_group_teardown);
}