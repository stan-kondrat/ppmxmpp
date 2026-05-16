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
#include "xmpp.h"
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

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int message_test_setup(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  return 0;
}

static int message_test_teardown(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  storage_db_close();
  return 0;
}

/* Drive a second user (bob/testpass) to ONLINE and register with a sink. */
static int feed_bob_to_online(xmpp_session_t* ctx, const char* resource) {
  sqlite3* db;
  if (storage_db_open(&db) == 0) {
    storage_users_create("bob@localhost", "testpass");
    storage_db_close();
  }

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
  if (ctx->state != XMPP_STATE_STARTTLS_SENT) return -1;

  simulate_starttls(ctx);

  const char* r1 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  if (xmpp_feed(ctx, r1, strlen(r1), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_FEATURES_RECEIVED_POST_TLS) return -1;

  if (feed_sasl_plain(ctx, "", "bob", "testpass") != 0) return -1;

  const char* r2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  if (xmpp_feed(ctx, r2, strlen(r2), mock_write, NULL) != 0) return -1;

  char bind[256];
  snprintf(bind, sizeof(bind),
           "<iq type='set' id='b1'>"
           "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
           "<resource>%s</resource></bind></iq>",
           resource);
  if (xmpp_feed(ctx, bind, strlen(bind), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_ONLINE) return -1;

  g_write_len = 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

/* A (testuser/test) sends a chat message to B (bob/phone) by full JID.
 * B's sink must receive the message with a correct from= attribute. */
static void test_message_a_to_b_full_jid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Bring B online and register with its own sink. */
  write_sink_t b_sink = {.len = 0};
  xmpp_session_t b_ctx;
  assert_int_equal(feed_bob_to_online(&b_ctx, "phone"), 0);
  xmpp_session_table_register(&b_ctx, sink_write, &b_sink);

  /* Bring A online. */
  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);

  /* A sends presence so it is registered. */
  const char* pres = "<presence/>";
  assert_int_equal(xmpp_feed(&a_ctx, pres, strlen(pres), mock_write, NULL), 0);

  /* A sends a chat message to B's full JID. */
  const char* msg = "<message to='bob@localhost/phone' type='chat' id='m1'>"
                    "<body>hello</body></message>";
  assert_int_equal(xmpp_feed(&a_ctx, msg, strlen(msg), mock_write, NULL), 0);

  /* B's sink must contain the message with from= set to A's full JID. */
  assert_true(sink_contains(&b_sink, "from='testuser@localhost/test'"));
  assert_true(sink_contains(&b_sink, "<body>hello</body>"));

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  xmpp_session_table_unregister("bob@localhost/phone");
  teardown_test_db();
}

/* A sends to B's bare JID; B has one resource → B receives it. */
static void test_message_a_to_b_bare_jid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t b_sink = {.len = 0};
  xmpp_session_t b_ctx;
  assert_int_equal(feed_bob_to_online(&b_ctx, "laptop"), 0);
  xmpp_session_table_register(&b_ctx, sink_write, &b_sink);

  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);

  const char* pres = "<presence/>";
  xmpp_feed(&a_ctx, pres, strlen(pres), mock_write, NULL);

  const char* msg = "<message to='bob@localhost' type='chat' id='m2'>"
                    "<body>hi bare</body></message>";
  assert_int_equal(xmpp_feed(&a_ctx, msg, strlen(msg), mock_write, NULL), 0);

  assert_true(sink_contains(&b_sink, "<body>hi bare</body>"));
  assert_true(sink_contains(&b_sink, "from='testuser@localhost/test'"));

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  xmpp_session_table_unregister("bob@localhost/laptop");
  teardown_test_db();
}

/* B has two resources. The one with higher priority wins.
 * If priorities are equal, the most recently registered wins. */
static void test_message_bare_jid_priority_tiebreak(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Register b_phone (priority 0, registered first). */
  write_sink_t b_phone_sink = {.len = 0};
  xmpp_session_t b_phone;
  assert_int_equal(feed_bob_to_online(&b_phone, "phone"), 0);
  xmpp_session_table_register(&b_phone, sink_write, &b_phone_sink);

  /* Register b_pc with higher priority. */
  write_sink_t b_pc_sink = {.len = 0};
  xmpp_session_t b_pc;
  assert_int_equal(feed_bob_to_online(&b_pc, "pc"), 0);
  xmpp_session_table_register(&b_pc, sink_write, &b_pc_sink);
  xmpp_session_table_update_priority("bob@localhost/pc", 5);

  /* A comes online. */
  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);
  const char* pres = "<presence/>";
  xmpp_feed(&a_ctx, pres, strlen(pres), mock_write, NULL);

  const char* msg = "<message to='bob@localhost' type='chat' id='m3'>"
                    "<body>priority</body></message>";
  assert_int_equal(xmpp_feed(&a_ctx, msg, strlen(msg), mock_write, NULL), 0);

  /* pc (priority=5) wins; phone should not receive. */
  assert_true(sink_contains(&b_pc_sink, "<body>priority</body>"));
  assert_int_equal(b_phone_sink.len, 0);

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_phone);
  xmpp_session_cleanup(&b_pc);
  xmpp_session_table_unregister("bob@localhost/phone");
  xmpp_session_table_unregister("bob@localhost/pc");
  teardown_test_db();
}

/* A sends to its own bare JID with two resources.
 * The OTHER resource receives; the sender does not. */
static void test_message_to_self_bare_jid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* A on "test" (feed_to_online uses resource "test"). */
  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);
  write_sink_t a_sink = {.len = 0};
  xmpp_session_table_register(&a_ctx, sink_write, &a_sink);

  /* A's second resource "mobile". */
  write_sink_t a_mobile_sink = {.len = 0};
  xmpp_session_t a_mobile;
  /* Reuse feed_bob_to_online pattern but for testuser. */
  memset(&a_mobile, 0, sizeof(a_mobile));
  xmpp_session_reset(&a_mobile);

  char buf[512];
  snprintf(buf, sizeof(buf),
           "<?xml version='1.0'?>"
           "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
           " xmlns='jabber:client' to='localhost' version='1.0'>");
  assert_int_equal(xmpp_feed(&a_mobile, buf, strlen(buf), mock_write, NULL), 0);
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  assert_int_equal(xmpp_feed(&a_mobile, starttls, strlen(starttls), mock_write, NULL), 0);
  simulate_starttls(&a_mobile);
  const char* r1 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  assert_int_equal(xmpp_feed(&a_mobile, r1, strlen(r1), mock_write, NULL), 0);
  assert_int_equal(feed_sasl_plain(&a_mobile, "", "testuser", "testpass"), 0);
  const char* r2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  assert_int_equal(xmpp_feed(&a_mobile, r2, strlen(r2), mock_write, NULL), 0);
  const char* bind2 = "<iq type='set' id='b2'>"
                      "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                      "<resource>mobile</resource></bind></iq>";
  assert_int_equal(xmpp_feed(&a_mobile, bind2, strlen(bind2), mock_write, NULL), 0);
  assert_int_equal(a_mobile.state, XMPP_STATE_ONLINE);
  xmpp_session_table_register(&a_mobile, sink_write, &a_mobile_sink);

  /* A (test) sends to own bare JID. */
  const char* msg = "<message to='testuser@localhost' type='chat' id='self1'>"
                    "<body>self message</body></message>";
  assert_int_equal(xmpp_feed(&a_ctx, msg, strlen(msg), sink_write, &a_sink), 0);

  /* The mobile resource should receive it. */
  assert_true(sink_contains(&a_mobile_sink, "<body>self message</body>"));

  /* The sender (test resource) must not receive its own message. */
  assert_false(sink_contains(&a_sink, "<body>self message</body>"));

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&a_mobile);
  xmpp_session_table_unregister("testuser@localhost/test");
  xmpp_session_table_unregister("testuser@localhost/mobile");
  teardown_test_db();
}

/* Message to a bare JID with no online session → stored offline (XEP-0160). */
static void test_message_to_offline_user_stored(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);
  const char* pres = "<presence/>";
  xmpp_feed(&a_ctx, pres, strlen(pres), mock_write, NULL);
  g_write_len = 0;

  const char* msg = "<message to='nobody@localhost' type='chat' id='m5'>"
                    "<body>gone</body></message>";
  assert_int_equal(xmpp_feed(&a_ctx, msg, strlen(msg), mock_write, NULL), 0);


  /* No error stanza should be returned; no write to sender. */
  assert_int_equal(g_write_len, 0);

  /* Message should be stored in offline_messages table. */
  assert_int_equal(offline_count("nobody@localhost"), 1);

  xmpp_session_cleanup(&a_ctx);
  teardown_test_db();
}

/* Missing to= attribute → sender gets a bad-request error. */
static void test_message_missing_to_returns_error(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);
  g_write_len = 0;

  const char* msg = "<message type='chat' id='m6'><body>oops</body></message>";
  assert_int_equal(xmpp_feed(&a_ctx, msg, strlen(msg), mock_write, NULL), 0);

  assert_non_null(buf_contains("type='error'"));
  assert_non_null(buf_contains("bad-request"));

  xmpp_session_cleanup(&a_ctx);
  teardown_test_db();
}

/* Reply: B sends a message back to A; A receives it. */
static void test_message_reply_b_to_a(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t a_sink = {.len = 0};
  write_sink_t b_sink = {.len = 0};

  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);

  xmpp_session_t b_ctx;
  assert_int_equal(feed_bob_to_online(&b_ctx, "pc"), 0);

  /* Feed initial presence through the per-session sink so ctx->write_fn stays as
   * the sink callback — xmpp_session_table_register uses ctx->write_fn. */
  const char* pres = "<presence/>";
  assert_int_equal(xmpp_feed(&a_ctx, pres, strlen(pres), sink_write, &a_sink), 0);
  assert_int_equal(xmpp_feed(&b_ctx, pres, strlen(pres), sink_write, &b_sink), 0);

  /* B replies to A by full JID. */
  const char* reply = "<message to='testuser@localhost/test' type='chat' id='r1'>"
                      "<body>hey back</body></message>";
  assert_int_equal(xmpp_feed(&b_ctx, reply, strlen(reply), sink_write, &b_sink), 0);

  assert_true(sink_contains(&a_sink, "<body>hey back</body>"));
  assert_true(sink_contains(&a_sink, "from='bob@localhost/pc'"));

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  xmpp_session_table_unregister("testuser@localhost/test");
  xmpp_session_table_unregister("bob@localhost/pc");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test_setup_teardown(test_message_a_to_b_full_jid, message_test_setup,
                                      message_test_teardown),
      cmocka_unit_test_setup_teardown(test_message_a_to_b_bare_jid, message_test_setup,
                                      message_test_teardown),
      cmocka_unit_test_setup_teardown(test_message_bare_jid_priority_tiebreak, message_test_setup,
                                      message_test_teardown),
      cmocka_unit_test_setup_teardown(test_message_to_self_bare_jid, message_test_setup,
                                      message_test_teardown),
      cmocka_unit_test_setup_teardown(test_message_to_offline_user_stored,
                                      message_test_setup, message_test_teardown),
      cmocka_unit_test_setup_teardown(test_message_missing_to_returns_error, message_test_setup,
                                      message_test_teardown),
      cmocka_unit_test_setup_teardown(test_message_reply_b_to_a, message_test_setup,
                                      message_test_teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
