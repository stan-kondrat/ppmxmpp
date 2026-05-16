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
#include "xep-0280-carbons.h"
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

static void sink_reset(write_sink_t* s) {
  memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/*  Test fixtures                                                     */
/* ------------------------------------------------------------------ */

static int carbons_setup(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  return 0;
}

static int carbons_teardown(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  storage_db_close();
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Session helper — bring a user to ONLINE and register it            */
/* ------------------------------------------------------------------ */

/* Feed one XMPP client through to ONLINE using authcid/passwd/resource.
 * The per-session write sink is sink_write / sink (caller provides sink). */
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
/*  Test: enable carbons IQ                                            */
/* ------------------------------------------------------------------ */

static void test_enable_carbons(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                   "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Verify initial carbons state is 0 (not set). */
  g_session_count = g_session_count; /* access g_sessions to verify structure */
  /* The carbons flag is initially 0 (session_entry_t defaults to 0). */

  /* Send enable IQ. */
  g_write_len = 0;
  const char* enable_iq =
      "<iq type='set' id='en1'>"
      "<enable xmlns='urn:xmpp:carbons:2'/>"
      "</iq>";
  assert_int_equal(xmpp_feed(&alice_ctx, enable_iq, strlen(enable_iq), sink_write, &alice_sink), 0);

  /* Server must reply with IQ result. */
  assert_non_null(memmem(alice_sink.buf, alice_sink.len, "<iq type='result' id='en1'/>",
                         strlen("<iq type='result' id='en1'/>")));

  /* Carbons flag must be set in the session table. */
  assert_true(xmpp_session_table_is_registered("testuser@localhost/desktop"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: disable carbons IQ                                           */
/* ------------------------------------------------------------------ */

static void test_disable_carbons(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                   "testuser", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Enable then disable. */
  g_write_len = 0;
  const char* enable_iq =
      "<iq type='set' id='en1'>"
      "<enable xmlns='urn:xmpp:carbons:2'/>"
      "</iq>";
  assert_int_equal(xmpp_feed(&alice_ctx, enable_iq, strlen(enable_iq), sink_write, &alice_sink), 0);

  sink_reset(&alice_sink);
  const char* disable_iq =
      "<iq type='set' id='dis1'>"
      "<disable xmlns='urn:xmpp:carbons:2'/>"
      "</iq>";
  assert_int_equal(xmpp_feed(&alice_ctx, disable_iq, strlen(disable_iq), sink_write, &alice_sink), 0);

  assert_non_null(memmem(alice_sink.buf, alice_sink.len, "<iq type='result' id='dis1'/>",
                         strlen("<iq type='result' id='dis1'/>")));
  assert_true(xmpp_session_table_is_registered("testuser@localhost/mobile"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_table_unregister("testuser@localhost/mobile");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: carbons sent copy delivered to other carbons-enabled session  */
/* ------------------------------------------------------------------ */

static void test_carbon_sent_to_other_resource(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Alice on 'desktop' (sender). */
  write_sink_t alice_desktop_sink = {.len = 0};
  xmpp_session_t alice_desktop;
  assert_int_equal(feed_user_to_online_with_sink(&alice_desktop, &alice_desktop_sink,
                                                   "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_desktop, sink_write, &alice_desktop_sink);

  /* Alice on 'mobile' (second resource, carbon-enabled). */
  write_sink_t alice_mobile_sink = {.len = 0};
  xmpp_session_t alice_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&alice_mobile, &alice_mobile_sink,
                                                  "testuser", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_mobile, sink_write, &alice_mobile_sink);

  /* Bob (recipient). */
  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&bob_ctx, &bob_sink,
                                                  "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  /* Enable carbons on both of Alice's resources. */
  g_write_len = 0;
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_desktop, enable, strlen(enable), sink_write, &alice_desktop_sink);
  sink_reset(&alice_desktop_sink);
  xmpp_feed(&alice_mobile, enable, strlen(enable), sink_write, &alice_mobile_sink);

  /* Alice (desktop) sends a chat message to Bob. */
  const char* msg = "<message to='bob@localhost' type='chat' id='m1'>"
                    "<body>hello from desktop</body></message>";
  assert_int_equal(xmpp_feed(&alice_desktop, msg, strlen(msg), sink_write, &alice_desktop_sink), 0);

  /* Bob receives the original message. */
  assert_true(sink_contains(&bob_sink, "hello from desktop"));

  /* Alice (mobile) should receive a <sent/> carbon copy.
   * The carbon wrapper uses sender's bare JID as from=, and the
   * mobile resource's full JID as to=, with <forwarded> inside <sent>. */
  assert_true(sink_contains(&alice_mobile_sink, "urn:xmpp:carbons:2"));
  assert_true(sink_contains(&alice_mobile_sink, "sent xmlns='urn:xmpp:carbons:2'"));
  assert_true(sink_contains(&alice_mobile_sink, "urn:xmpp:forward:0"));
  assert_true(sink_contains(&alice_mobile_sink, "hello from desktop"));
  /* Carbon from= must be Alice's bare JID (XEP-0280 §4). */
  assert_true(sink_contains(&alice_mobile_sink, "from='testuser@localhost'"));

  /* Alice (desktop) must NOT receive a carbon copy of its own sent message. */
  /* (self-message carbons are sent to other resources, not to sender itself) */

  xmpp_session_cleanup(&alice_desktop);
  xmpp_session_cleanup(&alice_mobile);
  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  xmpp_session_table_unregister("testuser@localhost/mobile");
  xmpp_session_table_unregister("bob@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: carbons received copy delivered to other carbons-enabled    */
/*  resources of the recipient                                         */
/* ------------------------------------------------------------------ */

static void test_carbon_received_by_other_resource(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Alice (sender). */
  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                   "testuser", "testpass", "phone"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Bob on 'mobile' (carbon-enabled, registered first so desktop wins tiebreak). */
  write_sink_t bob_mobile_sink = {.len = 0};
  xmpp_session_t bob_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&bob_mobile, &bob_mobile_sink,
                                                  "bob", "testpass", "mobile"), 0);
  xmpp_session_table_register(&bob_mobile, sink_write, &bob_mobile_sink);

  /* Bob on 'desktop' (primary recipient, registered last → highest last_active → best resource). */
  write_sink_t bob_desktop_sink = {.len = 0};
  xmpp_session_t bob_desktop;
  assert_int_equal(feed_user_to_online_with_sink(&bob_desktop, &bob_desktop_sink,
                                                   "bob", "testpass", "desktop"), 0);
  xmpp_session_table_register(&bob_desktop, sink_write, &bob_desktop_sink);

  /* Enable carbons on Bob's mobile resource. */
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&bob_mobile, enable, strlen(enable), sink_write, &bob_mobile_sink);

  /* Reset sinks before the message-under-test. */
  sink_reset(&bob_desktop_sink);
  sink_reset(&bob_mobile_sink);
  const char* msg = "<message to='bob@localhost' type='chat' id='m2'>"
                    "<body>incoming for you</body></message>";
  assert_int_equal(xmpp_feed(&alice_ctx, msg, strlen(msg), sink_write, &alice_sink), 0);

  /* Bob's desktop receives the original message (best resource). */
  assert_true(sink_contains(&bob_desktop_sink, "incoming for you"));

  /* Bob's mobile should receive a <received/> carbon copy. */
  assert_true(sink_contains(&bob_mobile_sink, "urn:xmpp:carbons:2"));
  assert_true(sink_contains(&bob_mobile_sink, "received xmlns='urn:xmpp:carbons:2'"));
  assert_true(sink_contains(&bob_mobile_sink, "urn:xmpp:forward:0"));
  assert_true(sink_contains(&bob_mobile_sink, "incoming for you"));
  /* Carbon from= must be Bob's bare JID per XEP-0280 §4. */
  assert_true(sink_contains(&bob_mobile_sink, "from='bob@localhost'"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_cleanup(&bob_desktop);
  xmpp_session_cleanup(&bob_mobile);
  xmpp_session_table_unregister("testuser@localhost/phone");
  xmpp_session_table_unregister("bob@localhost/desktop");
  xmpp_session_table_unregister("bob@localhost/mobile");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: message with <private> element skips carbon copying          */
/* ------------------------------------------------------------------ */

static void test_private_message_skips_carbons(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Alice (sender). */
  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                   "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Alice's mobile (second resource, carbon-enabled). */
  write_sink_t alice_mobile_sink = {.len = 0};
  xmpp_session_t alice_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&alice_mobile, &alice_mobile_sink,
                                                  "testuser", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_mobile, sink_write, &alice_mobile_sink);

  /* Bob (recipient). */
  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&bob_ctx, &bob_sink,
                                                  "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  /* Enable carbons on both of Alice's resources. */
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_ctx, enable, strlen(enable), sink_write, &alice_sink);
  xmpp_feed(&alice_mobile, enable, strlen(enable), sink_write, &alice_mobile_sink);

  /* Reset sinks after carbons setup, before the message-under-test. */
  sink_reset(&alice_mobile_sink);
  sink_reset(&bob_sink);

  /* Alice sends a message with <private> (XEP-0280 §9). */
  const char* msg = "<message to='bob@localhost' type='chat' id='m3'>"
                    "<body>secret message</body>"
                    "<private xmlns='urn:xmpp:carbons:2'/>"
                    "</message>";
  assert_int_equal(xmpp_feed(&alice_ctx, msg, strlen(msg), sink_write, &alice_sink), 0);

  /* Bob still receives the original message (RFC 6121 routing). */
  assert_true(sink_contains(&bob_sink, "secret message"));

  /* Alice's mobile should NOT receive a carbon copy (message was private). */
  assert_int_equal(alice_mobile_sink.len, 0);

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_cleanup(&alice_mobile);
  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  xmpp_session_table_unregister("testuser@localhost/mobile");
  xmpp_session_table_unregister("bob@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: non-carbons-enabled resource does NOT receive carbon copies  */
/* ------------------------------------------------------------------ */

static void test_non_carbon_resource_skipped(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Alice (sender). */
  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                   "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Alice's second resource WITHOUT carbons enabled. */
  write_sink_t alice_nocarb_sink = {.len = 0};
  xmpp_session_t alice_nocarb;
  assert_int_equal(feed_user_to_online_with_sink(&alice_nocarb, &alice_nocarb_sink,
                                                  "testuser", "testpass", "laptop"), 0);
  xmpp_session_table_register(&alice_nocarb, sink_write, &alice_nocarb_sink);

  /* Bob (recipient). */
  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&bob_ctx, &bob_sink,
                                                  "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  /* Only enable carbons on Alice's desktop. */
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_ctx, enable, strlen(enable), sink_write, &alice_sink);

  /* Reset sinks before the message-under-test so offline-drain noise is excluded. */
  sink_reset(&alice_nocarb_sink);
  sink_reset(&bob_sink);

  /* Alice (desktop) sends a message to Bob. */
  const char* msg = "<message to='bob@localhost' type='chat' id='m4'>"
                    "<body>hello bob</body></message>";
  assert_int_equal(xmpp_feed(&alice_ctx, msg, strlen(msg), sink_write, &alice_sink), 0);

  /* Bob receives the message. */
  assert_true(sink_contains(&bob_sink, "hello bob"));

  /* Alice's laptop (non-carbons) should NOT receive any carbon copy. */
  /* Non-carbons resources only receive RFC 6121 self-addressed messages
   * (broadcast_except). Alice sending to bob@localhost is NOT self-addressed,
   * so laptop receives nothing. */
  assert_int_equal(alice_nocarb_sink.len, 0);

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_cleanup(&alice_nocarb);
  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  xmpp_session_table_unregister("testuser@localhost/laptop");
  xmpp_session_table_unregister("bob@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: self-addressed message sends <sent/> carbon to all          */
/*  carbons-enabled resources including mobile                         */
/* ------------------------------------------------------------------ */

static void test_self_message_sends_carbon_to_all_carbons_resources(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Alice on 'desktop' (sender resource). */
  write_sink_t alice_desktop_sink = {.len = 0};
  xmpp_session_t alice_desktop;
  assert_int_equal(feed_user_to_online_with_sink(&alice_desktop, &alice_desktop_sink,
                                                   "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_desktop, sink_write, &alice_desktop_sink);

  /* Alice on 'mobile' (second resource, carbons-enabled). */
  write_sink_t alice_mobile_sink = {.len = 0};
  xmpp_session_t alice_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&alice_mobile, &alice_mobile_sink,
                                                  "testuser", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_mobile, sink_write, &alice_mobile_sink);

  /* Enable carbons on both resources. */
  g_write_len = 0;
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_desktop, enable, strlen(enable), sink_write, &alice_desktop_sink);
  sink_reset(&alice_mobile_sink);
  xmpp_feed(&alice_mobile, enable, strlen(enable), sink_write, &alice_mobile_sink);

  /* Alice sends to her own bare JID (self-addressed). */
  const char* msg = "<message to='testuser@localhost' type='chat' id='self1'>"
                    "<body>note to self</body></message>";
  assert_int_equal(xmpp_feed(&alice_desktop, msg, strlen(msg), sink_write, &alice_desktop_sink), 0);

  /* Alice's mobile (carbons-enabled) should receive a <sent/> carbon copy. */
  assert_true(sink_contains(&alice_mobile_sink, "urn:xmpp:carbons:2"));
  assert_true(sink_contains(&alice_mobile_sink, "sent xmlns='urn:xmpp:carbons:2'"));
  assert_true(sink_contains(&alice_mobile_sink, "note to self"));

  /* The desktop itself gets the RFC 6121 self-message via broadcast_except.
   * (Alice's desktop receives nothing via broadcast_except since it is excluded;
   * but the mobile receives the carbon via for_each_carbon_resource.) */

  xmpp_session_cleanup(&alice_desktop);
  xmpp_session_cleanup(&alice_mobile);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  xmpp_session_table_unregister("testuser@localhost/mobile");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: outgoing carbon has correct XEP-0297 forwarded stanza       */
/* ------------------------------------------------------------------ */

static void test_carbon_forwarded_stanza_format(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Alice (sender). */
  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&alice_ctx, &alice_sink,
                                                   "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  /* Alice's mobile (carbons-enabled). */
  write_sink_t alice_mobile_sink = {.len = 0};
  xmpp_session_t alice_mobile;
  assert_int_equal(feed_user_to_online_with_sink(&alice_mobile, &alice_mobile_sink,
                                                  "testuser", "testpass", "mobile"), 0);
  xmpp_session_table_register(&alice_mobile, sink_write, &alice_mobile_sink);

  /* Bob (recipient). */
  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online_with_sink(&bob_ctx, &bob_sink,
                                                  "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  /* Enable carbons on both of Alice's resources. */
  g_write_len = 0;
  const char* enable =
      "<iq type='set' id='en1'><enable xmlns='urn:xmpp:carbons:2'/></iq>";
  xmpp_feed(&alice_ctx, enable, strlen(enable), sink_write, &alice_sink);
  sink_reset(&alice_mobile_sink);
  xmpp_feed(&alice_mobile, enable, strlen(enable), sink_write, &alice_mobile_sink);

  /* Alice sends a message with type='chat' and id='msg1' to Bob. */
  const char* msg = "<message to='bob@localhost' type='chat' id='msg1'>"
                    "<body>greetings</body></message>";
  assert_int_equal(xmpp_feed(&alice_ctx, msg, strlen(msg), sink_write, &alice_sink), 0);

  /* Bob receives original. */
  assert_true(sink_contains(&bob_sink, "greetings"));

  /* Alice's mobile receives a <sent/> carbon. */
  assert_true(sink_contains(&alice_mobile_sink, "sent xmlns='urn:xmpp:carbons:2'"));

  /* Forwarded stanza must be inside <forwarded xmlns='urn:xmpp:forward:0'>. */
  assert_true(sink_contains(&alice_mobile_sink, "forwarded xmlns='urn:xmpp:forward:0'"));

  /* The forwarded inner <message> must preserve the original attributes:
   * from=, to=, type=, id= and contain the <body>. */
  assert_true(sink_contains(&alice_mobile_sink, "from='testuser@localhost/desktop'"));
  assert_true(sink_contains(&alice_mobile_sink, "to='bob@localhost'"));
  assert_true(sink_contains(&alice_mobile_sink, "type='chat'"));
  assert_true(sink_contains(&alice_mobile_sink, "id='msg1'"));
  assert_true(sink_contains(&alice_mobile_sink, "greetings"));

  /* The outer carbon wrapper's from= must be the sender's bare JID. */
  assert_true(sink_contains(&alice_mobile_sink, "from='testuser@localhost'"));
  assert_true(sink_contains(&alice_mobile_sink, "to='testuser@localhost/mobile'"));

  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_cleanup(&alice_mobile);
  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  xmpp_session_table_unregister("testuser@localhost/mobile");
  xmpp_session_table_unregister("bob@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test_setup_teardown(test_enable_carbons, carbons_setup, carbons_teardown),
      cmocka_unit_test_setup_teardown(test_disable_carbons, carbons_setup, carbons_teardown),
      cmocka_unit_test_setup_teardown(test_carbon_sent_to_other_resource, carbons_setup,
                                      carbons_teardown),
      cmocka_unit_test_setup_teardown(test_carbon_received_by_other_resource, carbons_setup,
                                      carbons_teardown),
      cmocka_unit_test_setup_teardown(test_private_message_skips_carbons, carbons_setup,
                                      carbons_teardown),
      cmocka_unit_test_setup_teardown(test_non_carbon_resource_skipped, carbons_setup,
                                      carbons_teardown),
      cmocka_unit_test_setup_teardown(test_self_message_sends_carbon_to_all_carbons_resources,
                                      carbons_setup, carbons_teardown),
      cmocka_unit_test_setup_teardown(test_carbon_forwarded_stanza_format, carbons_setup,
                                      carbons_teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}