/* XEP-0333 Chat Markers — unit tests
 * https://xmpp.org/extensions/xep-0333.html
 *
 * XEP-0333 v1.0.0 (2024-04-17) — "Displayed Markers" (renamed from "Chat Markers")
 * §2  <markable/> — client signals it wants displayed markers for this message.
 * §3  <displayed id='msg-id'/> — message shown to user (only remaining marker type).
 *
 * <received/> and <acknowledged/> were removed in v1.0.0 but are still
 * recognized here for backward compatibility with pre-v1.0.0 clients.
 *
 * Covers:
 *   §2  <markable/> detection via xep0333_has_markable()
 *   §3  <displayed/> + legacy <received/>/<acknowledged/> detection + id extraction
 *   Non-marker children do not trigger a match.
 *   Wrong namespace does not trigger a match.
 */
#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/db.h"
#include "storage/db_users.h"
#include "test_xmpp_helpers.h"
#include "xep-0333-chat-markers.h"
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

/* ------------------------------------------------------------------ */
/*  Test fixtures                                                     */
/* ------------------------------------------------------------------ */

static int markers_setup(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  return 0;
}

static int markers_teardown(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  storage_db_close();
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Build a minimal session, bring it ONLINE, register it in the table. */
static int feed_user_to_online(xmpp_session_t* ctx, write_sink_t* sink,
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

  snprintf(buf, sizeof(buf),
           "<iq type='set' id='b1'>"
           "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
           "<resource>%s</resource></bind></iq>",
           resource);
  if (xmpp_feed(ctx, buf, strlen(buf), sink_write, sink) != 0) return -1;
  if (ctx->state != XMPP_STATE_ONLINE) return -1;

  return 0;
}



/* ------------------------------------------------------------------ */
/*  Test: <markable/> is detected via xep0333_has_markable()            */
/* ------------------------------------------------------------------ */

static void test_markable_detected(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t alice_sink = {.len = 0};
  xmpp_session_t alice_ctx;
  assert_int_equal(feed_user_to_online(&alice_ctx, &alice_sink,
                                        "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&alice_ctx, sink_write, &alice_sink);

  xmpp_ctx_t* xctx = (xmpp_ctx_t*)alice_ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");
  xmpp_stanza_set_attribute(stanza, "to", "bob@localhost");
  xmpp_stanza_set_attribute(stanza, "type", "chat");
  xmpp_stanza_set_attribute(stanza, "id", "m1");
  xmpp_stanza_set_attribute(stanza, "xmlns", "jabber:client");

  xmpp_stanza_t* body = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(body, "body");
  xmpp_stanza_add_child(stanza, body);
  xmpp_stanza_release(body);

  xmpp_stanza_t* markable = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(markable, "markable");
  xmpp_stanza_set_ns(markable, "urn:xmpp:chat-markers:0");
  xmpp_stanza_add_child(stanza, markable);
  xmpp_stanza_release(markable);

  /* xep0333_has_markable returns 1 for <markable/> in the right namespace. */
  assert_int_equal(xep0333_has_markable(stanza), 1);
  /* xep0333_get_marker returns NULL for <markable/> because markable is NOT
   * a marker element — it signals intent, not a marker itself. */
  assert_null(xep0333_get_marker(stanza, NULL));

  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&alice_ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <markable/> in wrong namespace returns 0                      */
/* ------------------------------------------------------------------ */

static void test_markable_wrong_ns_returns_zero(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "phone"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);
  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");
  xmpp_stanza_t* markable = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(markable, "markable");
  xmpp_stanza_set_ns(markable, "urn:xmpp:receipts"); /* wrong namespace */
  xmpp_stanza_add_child(stanza, markable);
  xmpp_stanza_release(markable);
  assert_int_equal(xep0333_has_markable(stanza), 0);
  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: no markable in plain message                                 */
/* ------------------------------------------------------------------ */

static void test_no_markable_in_plain_message(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);
  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");
  xmpp_stanza_set_attribute(stanza, "to", "bob@localhost");
  xmpp_stanza_set_attribute(stanza, "type", "chat");
  xmpp_stanza_t* body = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(body, "body");
  xmpp_stanza_t* text = xmpp_stanza_new(xctx);
  xmpp_stanza_set_text(text, "hello");
  xmpp_stanza_add_child(body, text);
  xmpp_stanza_release(text);
  xmpp_stanza_add_child(stanza, body);
  xmpp_stanza_release(body);
  assert_int_equal(xep0333_has_markable(stanza), 0);
  assert_null(xep0333_get_marker(stanza, NULL));
  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: markable and marker element co-exist in same stanza          */
/* ------------------------------------------------------------------ */

static void test_markable_and_displayed_coexist(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "phone"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);
  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");
  xmpp_stanza_set_attribute(stanza, "to", "alice@localhost");
  xmpp_stanza_set_attribute(stanza, "type", "chat");
  xmpp_stanza_t* markable = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(markable, "markable");
  xmpp_stanza_set_ns(markable, "urn:xmpp:chat-markers:0");
  xmpp_stanza_add_child(stanza, markable);
  xmpp_stanza_release(markable);
  xmpp_stanza_t* displayed = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(displayed, "displayed");
  xmpp_stanza_set_ns(displayed, "urn:xmpp:chat-markers:0");
  xmpp_stanza_set_attribute(displayed, "id", "msg-abc");
  xmpp_stanza_add_child(stanza, displayed);
  xmpp_stanza_release(displayed);
  assert_int_equal(xep0333_has_markable(stanza), 1);
  const char* marker_name = NULL;
  const char* marker_id = NULL;
  marker_name = xep0333_get_marker(stanza, &marker_id);
  assert_non_null(marker_name);
  assert_string_equal(marker_name, "displayed");
  assert_non_null(marker_id);
  assert_string_equal(marker_id, "msg-abc");
  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <received/> marker detected                                  */
/* ------------------------------------------------------------------ */

static void test_received_marker_detected(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t bob_sink = {.len = 0};
  xmpp_session_t bob_ctx;
  assert_int_equal(feed_user_to_online(&bob_ctx, &bob_sink,
                                       "bob", "testpass", "phone"), 0);
  xmpp_session_table_register(&bob_ctx, sink_write, &bob_sink);

  xmpp_ctx_t* xctx = (xmpp_ctx_t*)bob_ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");
  xmpp_stanza_set_attribute(stanza, "to", "alice@localhost");
  xmpp_stanza_set_attribute(stanza, "type", "chat");
  xmpp_stanza_set_attribute(stanza, "id", "recv1");
  xmpp_stanza_set_attribute(stanza, "xmlns", "jabber:client");

  xmpp_stanza_t* received = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(received, "received");
  xmpp_stanza_set_ns(received, "urn:xmpp:chat-markers:0");
  xmpp_stanza_set_attribute(received, "id", "msg-abc");
  xmpp_stanza_add_child(stanza, received);
  xmpp_stanza_release(received);

  const char* marker_name = NULL;
  const char* marker_id = NULL;
  marker_name = xep0333_get_marker(stanza, &marker_id);

  assert_non_null(marker_name);
  assert_string_equal(marker_name, "received");
  assert_non_null(marker_id);
  assert_string_equal(marker_id, "msg-abc");

  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&bob_ctx);
  xmpp_session_table_unregister("bob@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <displayed/> marker detected                                 */
/* ------------------------------------------------------------------ */

static void test_displayed_marker_detected(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "phone"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);

  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");
  xmpp_stanza_set_attribute(stanza, "to", "friend@localhost");

  xmpp_stanza_t* displayed = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(displayed, "displayed");
  xmpp_stanza_set_ns(displayed, "urn:xmpp:chat-markers:0");
  xmpp_stanza_set_attribute(displayed, "id", "msg-xyz");
  xmpp_stanza_add_child(stanza, displayed);
  xmpp_stanza_release(displayed);

  const char* marker_name = NULL;
  const char* marker_id = NULL;
  marker_name = xep0333_get_marker(stanza, &marker_id);

  assert_non_null(marker_name);
  assert_string_equal(marker_name, "displayed");
  assert_non_null(marker_id);
  assert_string_equal(marker_id, "msg-xyz");

  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <acknowledged/> marker detected                              */
/* ------------------------------------------------------------------ */

static void test_acknowledged_marker_detected(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "laptop"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);

  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");
  xmpp_stanza_set_attribute(stanza, "to", "alice@localhost");

  xmpp_stanza_t* ack = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(ack, "acknowledged");
  xmpp_stanza_set_ns(ack, "urn:xmpp:chat-markers:0");
  xmpp_stanza_set_attribute(ack, "id", "msg-def");
  xmpp_stanza_add_child(stanza, ack);
  xmpp_stanza_release(ack);

  const char* marker_name = NULL;
  const char* marker_id = NULL;
  marker_name = xep0333_get_marker(stanza, &marker_id);

  assert_non_null(marker_name);
  assert_string_equal(marker_name, "acknowledged");
  assert_non_null(marker_id);
  assert_string_equal(marker_id, "msg-def");

  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/laptop");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: <received/> in wrong namespace returns NULL                  */
/* ------------------------------------------------------------------ */

static void test_received_wrong_ns_returns_null(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "desktop"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);

  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");

  /* <received xmlns='urn:xmpp:receipts'> is XEP-0184, not XEP-0333. */
  xmpp_stanza_t* received = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(received, "received");
  xmpp_stanza_set_ns(received, "urn:xmpp:receipts");
  xmpp_stanza_set_attribute(received, "id", "receipt-only");
  xmpp_stanza_add_child(stanza, received);
  xmpp_stanza_release(received);

  const char* marker = xep0333_get_marker(stanza, NULL);
  assert_null(marker);

  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/desktop");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: marker with no id attribute                                  */
/* ------------------------------------------------------------------ */

static void test_marker_without_id(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "phone"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);

  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");

  xmpp_stanza_t* displayed = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(displayed, "displayed");
  xmpp_stanza_set_ns(displayed, "urn:xmpp:chat-markers:0");
  /* No id attribute set. */
  xmpp_stanza_add_child(stanza, displayed);
  xmpp_stanza_release(displayed);

  const char* marker_name = NULL;
  const char* marker_id = NULL;
  marker_name = xep0333_get_marker(stanza, &marker_id);

  assert_non_null(marker_name);
  assert_string_equal(marker_name, "displayed");
  assert_null(marker_id);

  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: marker id out pointer can be NULL                            */
/* ------------------------------------------------------------------ */

static void test_marker_id_out_can_be_null(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t sink = {.len = 0};
  xmpp_session_t ctx;
  assert_int_equal(feed_user_to_online(&ctx, &sink, "testuser", "testpass", "phone"), 0);
  xmpp_session_table_register(&ctx, sink_write, &sink);

  xmpp_ctx_t* xctx = (xmpp_ctx_t*)ctx.strophe_ctx;
  xmpp_stanza_t* stanza = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(stanza, "message");

  xmpp_stanza_t* ack = xmpp_stanza_new(xctx);
  xmpp_stanza_set_name(ack, "acknowledged");
  xmpp_stanza_set_ns(ack, "urn:xmpp:chat-markers:0");
  xmpp_stanza_set_attribute(ack, "id", "id-ignore-me");
  xmpp_stanza_add_child(stanza, ack);
  xmpp_stanza_release(ack);

  /* Passing NULL for marker_id_out must not crash. */
  const char* marker = xep0333_get_marker(stanza, NULL);
  assert_non_null(marker);
  assert_string_equal(marker, "acknowledged");

  xmpp_stanza_release(stanza);
  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/phone");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: NULL stanza returns NULL                                     */
/* ------------------------------------------------------------------ */

static void test_null_stanza_returns_null(void** state) {
  (void)state;
  const char* marker = xep0333_get_marker(NULL, NULL);
  assert_null(marker);
  /* xep0333_has_markable(NULL) also returns 0, no crash. */
  assert_int_equal(xep0333_has_markable(NULL), 0);
}

/* ------------------------------------------------------------------ */
/*  main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test_setup_teardown(test_no_markable_in_plain_message,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_markable_detected,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_markable_wrong_ns_returns_zero,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_markable_and_displayed_coexist,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_received_marker_detected,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_displayed_marker_detected,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_acknowledged_marker_detected,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_received_wrong_ns_returns_null,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_marker_without_id,
                                      markers_setup, markers_teardown),
      cmocka_unit_test_setup_teardown(test_marker_id_out_can_be_null,
                                      markers_setup, markers_teardown),
      cmocka_unit_test(test_null_stanza_returns_null),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}