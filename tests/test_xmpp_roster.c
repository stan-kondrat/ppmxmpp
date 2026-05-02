#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "storage/db.h"
#include "storage/roster.h"
#include "test_xmpp_helpers.h"
#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Helper: drive a session to XMPP_STATE_CONNECTED                  */
/* ------------------------------------------------------------------ */

static int feed_stream_open_local(xmpp_session_t* ctx, const char* domain) {
  char buf[512];
  snprintf(buf, sizeof(buf),
           "<?xml version='1.0'?>"
           "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
           " xmlns='jabber:client' to='%s' version='1.0'>",
           domain);
  return xmpp_feed(ctx, buf, strlen(buf), mock_write, NULL);
}

static int feed_to_connected(xmpp_session_t* ctx) {
  xmpp_session_reset(ctx);
  g_write_len = 0;

  if (feed_stream_open_local(ctx, "localhost") != 0) return -1;
  if (ctx->state != XMPP_STATE_STREAM_OPENED_PLAINTEXT) return -1;

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  if (xmpp_feed(ctx, starttls, strlen(starttls), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_TLS_HANDSHAKING) return -1;

  g_write_len = 0;
  const char* r1 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  if (xmpp_feed(ctx, r1, strlen(r1), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_STREAM_OPENED_TLS) return -1;

  if (feed_sasl_plain(ctx, "", "testuser", "testpass") != 0) return -1;
  if (ctx->state != XMPP_STATE_STREAM_OPENED_AUTHENTICATED) return -1;

  g_write_len = 0;
  const char* r2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  if (xmpp_feed(ctx, r2, strlen(r2), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_RESOURCE_BOUND) return -1;

  g_write_len = 0;
  const char* bind = "<iq type='set' id='b1'>"
                     "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                     "<resource>test</resource></bind></iq>";
  if (xmpp_feed(ctx, bind, strlen(bind), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_CONNECTED) return -1;

  g_write_len = 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  1. Roster get on empty roster                                     */
/* ------------------------------------------------------------------ */

static void test_roster_get_empty(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq = "<iq type='get' id='r1'>"
                   "<query xmlns='jabber:iq:roster'/></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='result' id='r1'>"));
  assert_true(buf_contains("<query xmlns='jabber:iq:roster'"));
  assert_true(buf_contains("</query>"));
  /* No <item> elements for empty roster. */
  assert_false(buf_contains("<item"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  2. Roster set — add contact                                       */
/* ------------------------------------------------------------------ */

static void test_roster_set_add(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq =
      "<iq type='set' id='rs1'>"
      "<query xmlns='jabber:iq:roster'>"
      "<item jid='friend@example.com' name='Friend'/>"
      "</query></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  /* Acknowledge */
  assert_true(buf_contains("<iq type='result' id='rs1'/>"));
  /* Roster push to this resource */
  assert_true(buf_contains("jabber:iq:roster"));
  assert_true(buf_contains("friend@example.com"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  3. Roster get after add — item present                            */
/* ------------------------------------------------------------------ */

static void test_roster_get_after_add(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Pre-seed a roster item directly. */
  storage_roster_item_t item;
  memset(&item, 0, sizeof(item));
  strncpy(item.contact_jid, "bob@example.com", sizeof(item.contact_jid) - 1);
  strncpy(item.name, "Bob", sizeof(item.name) - 1);
  strncpy(item.subscription, "both", sizeof(item.subscription) - 1);
  const char* groups[] = { "Friends" };
  assert_int_equal(storage_roster_upsert("testuser@localhost", &item, groups, 1), 0);
  storage_db_close();

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq = "<iq type='get' id='r2'>"
                   "<query xmlns='jabber:iq:roster'/></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='result' id='r2'>"));
  assert_true(buf_contains("bob@example.com"));
  assert_true(buf_contains("Bob"));
  assert_true(buf_contains("subscription='both'"));
  assert_true(buf_contains("<group>Friends</group>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  4. Roster set — remove contact                                    */
/* ------------------------------------------------------------------ */

static void test_roster_set_remove(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Pre-seed. */
  storage_roster_item_t item;
  memset(&item, 0, sizeof(item));
  strncpy(item.contact_jid, "alice@example.com", sizeof(item.contact_jid) - 1);
  strncpy(item.subscription, "none", sizeof(item.subscription) - 1);
  assert_int_equal(storage_roster_upsert("testuser@localhost", &item, NULL, 0), 0);
  storage_db_close();

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq =
      "<iq type='set' id='rs2'>"
      "<query xmlns='jabber:iq:roster'>"
      "<item jid='alice@example.com' subscription='remove'/>"
      "</query></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='result' id='rs2'/>"));
  /* Push with subscription='remove' */
  assert_true(buf_contains("subscription='remove'"));
  assert_true(buf_contains("alice@example.com"));

  /* Verify DB removal. */
  storage_roster_item_t check;
  int gr = storage_roster_get("testuser@localhost", "alice@example.com", &check);
  assert_int_equal(gr, 1); /* not found */
  storage_db_close();

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  5. Roster set — multiple items rejected with bad-request          */
/* ------------------------------------------------------------------ */

static void test_roster_set_multiple_items(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq =
      "<iq type='set' id='rs3'>"
      "<query xmlns='jabber:iq:roster'>"
      "<item jid='a@example.com'/>"
      "<item jid='b@example.com'/>"
      "</query></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='error' id='rs3'>"));
  assert_true(buf_contains("<bad-request"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  6. Roster set — missing jid attribute → bad-request              */
/* ------------------------------------------------------------------ */

static void test_roster_set_missing_jid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq =
      "<iq type='set' id='rs4'>"
      "<query xmlns='jabber:iq:roster'>"
      "<item name='NoJid'/>"
      "</query></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='error' id='rs4'>"));
  assert_true(buf_contains("<bad-request"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  7. Unknown IQ namespace → feature-not-implemented                 */
/* ------------------------------------------------------------------ */

static void test_iq_unknown_namespace(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq =
      "<iq type='get' id='u1'>"
      "<query xmlns='jabber:iq:unknown'/></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='error' id='u1'>"));
  assert_true(buf_contains("<feature-not-implemented"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  8. Roster set with group                                          */
/* ------------------------------------------------------------------ */

static void test_roster_set_with_group(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_connected(&ctx), 0);

  const char* iq =
      "<iq type='set' id='rs5'>"
      "<query xmlns='jabber:iq:roster'>"
      "<item jid='carol@example.com' name='Carol'>"
      "<group>Work</group>"
      "</item>"
      "</query></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='result' id='rs5'/>"));
  assert_true(buf_contains("carol@example.com"));
  assert_true(buf_contains("<group>Work</group>"));

  /* Verify stored in DB. */
  const char* groups[8];
  int gc = storage_roster_get_groups("testuser@localhost", "carol@example.com", groups, 8);
  assert_int_equal(gc, 1);
  assert_string_equal(groups[0], "Work");
  storage_db_close();

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_roster_get_empty),
      cmocka_unit_test(test_roster_set_add),
      cmocka_unit_test(test_roster_get_after_add),
      cmocka_unit_test(test_roster_set_remove),
      cmocka_unit_test(test_roster_set_multiple_items),
      cmocka_unit_test(test_roster_set_missing_jid),
      cmocka_unit_test(test_iq_unknown_namespace),
      cmocka_unit_test(test_roster_set_with_group),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
