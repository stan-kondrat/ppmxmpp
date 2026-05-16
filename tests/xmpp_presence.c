#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "storage/db.h"
#include "storage/db_roster.h"
#include "test_xmpp_helpers.h"
#include "xmpp.h"
#include "xmpp_presence.h"
#include "xmpp_session.h"

/* ------------------------------------------------------------------ */
/*  Per-test write sink                                                */
/*                                                                     */
/*  Each "virtual client" gets its own sink so we can verify which    */
/*  sessions received which stanzas independently of g_write_buf.     */
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

/* Add a roster entry with a given subscription state. */
static int add_roster_entry(const char* owner, const char* contact, const char* subscription) {
  sqlite3* db;
  if (storage_db_open(&db) != 0) return -1;
  storage_roster_item_t item;
  memset(&item, 0, sizeof(item));
  strncpy(item.contact_jid, contact, sizeof(item.contact_jid) - 1);
  strncpy(item.subscription, subscription, sizeof(item.subscription) - 1);
  int rc = storage_roster_upsert(owner, &item, NULL, 0);
  storage_db_close();
  return rc;
}

static int presence_test_setup(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  return 0;
}

static int presence_test_teardown(void** state) {
  (void)state;
  xmpp_session_table_reset_all();
  storage_db_close();
  return 0;
}

/* Build a minimal xmpp_session_t with a bound_jid set, without going
 * through the full handshake — used for session registry tests that do
 * not need to feed XML through the parser. */
static void make_session(xmpp_session_t* ctx, const char* bound_jid) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->state = XMPP_STATE_ONLINE;
  strncpy(ctx->bound_jid, bound_jid, sizeof(ctx->bound_jid) - 1);
}

/* ------------------------------------------------------------------ */
/*  Session registry tests                                             */
/* ------------------------------------------------------------------ */

static void test_register_and_write(void** state) {
  (void)state;
  write_sink_t sink = {.len = 0};

  xmpp_session_t ctx;
  make_session(&ctx, "alice@localhost/res1");
  xmpp_session_table_register(&ctx, sink_write, &sink);

  int rc = xmpp_session_table_write("alice@localhost/res1", "hello", 5);
  assert_int_equal(rc, 0);
  assert_true(sink_contains(&sink, "hello"));

  xmpp_session_table_unregister("alice@localhost/res1");
}

static void test_write_unknown_session_returns_minus1(void** state) {
  (void)state;
  int rc = xmpp_session_table_write("nobody@localhost/x", "data", 4);
  assert_int_equal(rc, -1);
}

static void test_unregister_removes_session(void** state) {
  (void)state;
  write_sink_t sink = {.len = 0};

  xmpp_session_t ctx;
  make_session(&ctx, "bob@localhost/r1");
  xmpp_session_table_register(&ctx, sink_write, &sink);
  xmpp_session_table_unregister("bob@localhost/r1");

  int rc = xmpp_session_table_write("bob@localhost/r1", "x", 1);
  assert_int_equal(rc, -1);
}

static void test_re_register_updates_callbacks(void** state) {
  (void)state;
  write_sink_t sink1 = {.len = 0};
  write_sink_t sink2 = {.len = 0};

  xmpp_session_t ctx;
  make_session(&ctx, "carol@localhost/r");
  xmpp_session_table_register(&ctx, sink_write, &sink1);
  xmpp_session_table_register(&ctx, sink_write, &sink2);

  xmpp_session_table_write("carol@localhost/r", "hi", 2);
  assert_int_equal(sink1.len, 0);  /* old sink not written */
  assert_true(sink_contains(&sink2, "hi"));

  xmpp_session_table_unregister("carol@localhost/r");
}

/* ------------------------------------------------------------------ */
/*  Initial presence broadcast                                         */
/* ------------------------------------------------------------------ */

static void test_initial_presence_reaches_subscriber(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* testuser@localhost has contact2@localhost with subscription="from"
   * (contact2 subscribed to testuser's presence). */
  assert_int_equal(add_roster_entry("testuser@localhost", "contact2@localhost", "from"), 0);

  /* Register contact2's session. */
  write_sink_t contact2_sink = {.len = 0};
  xmpp_session_t contact2;
  make_session(&contact2, "contact2@localhost/phone");
  xmpp_session_table_register(&contact2, sink_write, &contact2_sink);

  /* testuser goes online via the full XMPP handshake. */
  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  const char* pres = "<presence/>";
  assert_int_equal(xmpp_feed(&ctx, pres, strlen(pres), mock_write, NULL), 0);

  /* contact2 should have received the presence broadcast. */
  assert_true(sink_contains(&contact2_sink, "<presence from='testuser@localhost/"));

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("contact2@localhost/phone");
  teardown_test_db();
}

static void test_initial_presence_not_sent_to_non_subscriber(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* subscription="to" means WE subscribed to THEM — they should NOT get our presence. */
  assert_int_equal(add_roster_entry("testuser@localhost", "other@localhost", "to"), 0);

  write_sink_t other_sink = {.len = 0};
  xmpp_session_t other;
  make_session(&other, "other@localhost/pc");
  xmpp_session_table_register(&other, sink_write, &other_sink);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  const char* pres = "<presence/>";
  xmpp_feed(&ctx, pres, strlen(pres), mock_write, NULL);

  assert_int_equal(other_sink.len, 0);

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("other@localhost/pc");
  teardown_test_db();
}

static void test_initial_presence_sent_to_both_subscription(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  assert_int_equal(add_roster_entry("testuser@localhost", "buddy@localhost", "both"), 0);

  write_sink_t buddy_sink = {.len = 0};
  xmpp_session_t buddy;
  make_session(&buddy, "buddy@localhost/web");
  xmpp_session_table_register(&buddy, sink_write, &buddy_sink);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  xmpp_feed(&ctx, "<presence/>", strlen("<presence/>"), mock_write, NULL);

  assert_true(sink_contains(&buddy_sink, "<presence from='testuser@localhost/"));

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("buddy@localhost/web");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Own other resources receive initial presence (RFC 6121 §4.2.2)   */
/* ------------------------------------------------------------------ */

static void test_initial_presence_sent_to_own_other_resource(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Register a second resource for testuser. */
  write_sink_t res2_sink = {.len = 0};
  xmpp_session_t res2;
  make_session(&res2, "testuser@localhost/mobile");
  xmpp_session_table_register(&res2, sink_write, &res2_sink);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  xmpp_feed(&ctx, "<presence/>", strlen("<presence/>"), mock_write, NULL);

  /* The other resource of the same user should receive the broadcast. */
  assert_true(sink_contains(&res2_sink, "<presence from='testuser@localhost/"));

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("testuser@localhost/mobile");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Unavailable presence broadcast (RFC 6121 §4.4)                   */
/* ------------------------------------------------------------------ */

static void test_unavailable_presence_reaches_subscriber(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  assert_int_equal(add_roster_entry("testuser@localhost", "watcher@localhost", "from"), 0);

  write_sink_t watcher_sink = {.len = 0};
  xmpp_session_t watcher;
  make_session(&watcher, "watcher@localhost/lap");
  xmpp_session_table_register(&watcher, sink_write, &watcher_sink);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  /* Send initial presence first so the session is considered available. */
  xmpp_feed(&ctx, "<presence/>", strlen("<presence/>"), mock_write, NULL);
  watcher_sink.len = 0;  /* reset to isolate the unavailable check */

  const char* unav = "<presence type='unavailable'/>";
  xmpp_feed(&ctx, unav, strlen(unav), mock_write, NULL);

  assert_true(sink_contains(&watcher_sink, "type='unavailable'"));
  assert_true(sink_contains(&watcher_sink, "from='testuser@localhost/"));

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("watcher@localhost/lap");
  teardown_test_db();
}

static void test_unavailable_unregisters_session(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  xmpp_feed(&ctx, "<presence/>", strlen("<presence/>"), mock_write, NULL);

  /* Sending unavailable should remove the session from the registry. */
  xmpp_feed(&ctx, "<presence type='unavailable'/>",
            strlen("<presence type='unavailable'/>"), mock_write, NULL);

  assert_int_equal(xmpp_session_table_write(ctx.bound_jid, "x", 1), -1);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Directed presence (RFC 6121 §4.6)                                 */
/* ------------------------------------------------------------------ */

static void test_directed_presence_delivered_to_full_jid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t target_sink = {.len = 0};
  xmpp_session_t target;
  make_session(&target, "friend@localhost/desk");
  xmpp_session_table_register(&target, sink_write, &target_sink);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  const char* dp = "<presence to='friend@localhost/desk'/>";
  xmpp_feed(&ctx, dp, strlen(dp), mock_write, NULL);

  assert_true(sink_contains(&target_sink, "from='testuser@localhost/"));
  assert_true(sink_contains(&target_sink, "to='friend@localhost/desk'"));

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("friend@localhost/desk");
  teardown_test_db();
}

static void test_directed_presence_delivered_to_bare_jid(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Two resources for the target bare JID. */
  write_sink_t sink_a = {.len = 0}, sink_b = {.len = 0};
  xmpp_session_t res_a, res_b;
  make_session(&res_a, "multi@localhost/a");
  make_session(&res_b, "multi@localhost/b");
  xmpp_session_table_register(&res_a, sink_write, &sink_a);
  xmpp_session_table_register(&res_b, sink_write, &sink_b);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  /* Directed to bare JID — both resources should receive it. */
  const char* dp = "<presence to='multi@localhost'/>";
  xmpp_feed(&ctx, dp, strlen(dp), mock_write, NULL);

  assert_true(sink_contains(&sink_a, "from='testuser@localhost/"));
  assert_true(sink_contains(&sink_b, "from='testuser@localhost/"));

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("multi@localhost/a");
  xmpp_session_table_unregister("multi@localhost/b");
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Disconnect triggers unavailable broadcast (RFC 6121 §4.4.2)       */
/* ------------------------------------------------------------------ */

static void test_disconnect_broadcasts_unavailable(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  assert_int_equal(add_roster_entry("testuser@localhost", "watch2@localhost", "from"), 0);

  write_sink_t watch2_sink = {.len = 0};
  xmpp_session_t watch2;
  make_session(&watch2, "watch2@localhost/x");
  xmpp_session_table_register(&watch2, sink_write, &watch2_sink);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  g_write_len = 0;

  /* Send initial presence so the session is registered as available. */
  xmpp_feed(&ctx, "<presence/>", strlen("<presence/>"), mock_write, NULL);
  watch2_sink.len = 0;

  /* Simulate ungraceful disconnect. */
  char bare_jid[64];
  const char* slash = strchr(ctx.bound_jid, '/');
  size_t blen = slash ? (size_t)(slash - ctx.bound_jid) : strlen(ctx.bound_jid);
  memcpy(bare_jid, ctx.bound_jid, blen);
  bare_jid[blen] = '\0';
  xmpp_presence_on_disconnect(ctx.bound_jid, bare_jid);

  assert_true(sink_contains(&watch2_sink, "type='unavailable'"));

  xmpp_session_cleanup(&ctx);
  xmpp_session_table_unregister("watch2@localhost/x");
  teardown_test_db();
}

static void test_disconnect_noop_when_never_sent_presence(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);
  /* Do NOT send <presence/> — session is bound but not registered in table. */

  xmpp_presence_on_disconnect(ctx.bound_jid, "testuser@localhost");

  /* Nothing should be written to any subscriber. */
  assert_int_equal(g_write_len, 0);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Subscription helpers                                               */
/* ------------------------------------------------------------------ */

#include "storage/db_users.h"

/* Drive a second user (contactuser/testpass) to XMPP_STATE_ONLINE.
 * setup_test_db() must have been called first. */
static int feed_contact_to_online(xmpp_session_t* ctx) {
  sqlite3* db;
  if (storage_db_open(&db) == 0) {
    storage_users_create("contactuser@localhost", "testpass");
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

  if (feed_sasl_plain(ctx, "", "contactuser", "testpass") != 0) return -1;

  const char* r2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  if (xmpp_feed(ctx, r2, strlen(r2), mock_write, NULL) != 0) return -1;

  const char* bind = "<iq type='set' id='cb1'>"
                     "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                     "<resource>ctest</resource></bind></iq>";
  if (xmpp_feed(ctx, bind, strlen(bind), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_ONLINE) return -1;

  g_write_len = 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Subscription tests (RFC 6121 §3)                                  */
/* ------------------------------------------------------------------ */

/* A sends subscribe to B — B receives the stanza; A's roster gets ask=subscribe. */
static void test_subscribe_delivered_to_online_target(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* B session (contactuser). */
  write_sink_t b_sink = {.len = 0};
  xmpp_session_t b_ctx;
  assert_int_equal(feed_contact_to_online(&b_ctx), 0);
  /* Re-register b_ctx with b_sink so subscription stanzas land there. */
  xmpp_session_table_register(&b_ctx, sink_write, &b_sink);

  /* A session (testuser). */
  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);

  /* A sends subscribe to B. */
  const char* sub = "<presence type='subscribe' to='contactuser@localhost'/>";
  assert_int_equal(xmpp_feed(&a_ctx, sub, strlen(sub), mock_write, NULL), 0);

  /* B should receive the subscribe stanza. */
  assert_true(sink_contains(&b_sink, "type='subscribe'"));
  assert_true(sink_contains(&b_sink, "from='testuser@localhost'"));

  /* A's roster should have ask=1 for contactuser. */
  storage_roster_item_t item;
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  int rc = storage_roster_get("testuser@localhost", "contactuser@localhost", &item);
  storage_db_close();
  assert_int_equal(rc, 0);
  assert_int_equal(item.ask, 1);

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  teardown_test_db();
}

/* B accepts: subscribed → both rosters updated; A gets subscribed stanza. */
static void test_subscribed_updates_both_rosters(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Seed: A's roster has B with ask=1, sub=none; B's roster has A with sub=none. */
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  storage_roster_item_t a_item;
  memset(&a_item, 0, sizeof(a_item));
  strncpy(a_item.contact_jid, "contactuser@localhost", sizeof(a_item.contact_jid) - 1);
  strncpy(a_item.subscription, "none", sizeof(a_item.subscription) - 1);
  a_item.ask = 1;
  storage_roster_upsert("testuser@localhost", &a_item, NULL, 0);
  storage_db_close();

  /* A session online to receive roster push. */
  write_sink_t a_sink = {.len = 0};
  xmpp_session_t a_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);
  xmpp_session_table_register(&a_ctx, sink_write, &a_sink);

  /* B session online — sends subscribed to A. */
  xmpp_session_t b_ctx;
  assert_int_equal(feed_contact_to_online(&b_ctx), 0);

  const char* accept = "<presence type='subscribed' to='testuser@localhost'/>";
  assert_int_equal(xmpp_feed(&b_ctx, accept, strlen(accept), mock_write, NULL), 0);

  /* A should receive 'subscribed' stanza. */
  assert_true(sink_contains(&a_sink, "type='subscribed'"));

  /* A's roster for B: subscription should now be 'to', ask cleared. */
  storage_roster_item_t updated_a;
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  int rc = storage_roster_get("testuser@localhost", "contactuser@localhost", &updated_a);
  storage_db_close();
  assert_int_equal(rc, 0);
  assert_string_equal(updated_a.subscription, "to");
  assert_int_equal(updated_a.ask, 0);

  /* B's roster for A: subscription should now be 'from'. */
  storage_roster_item_t updated_b;
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  rc = storage_roster_get("contactuser@localhost", "testuser@localhost", &updated_b);
  storage_db_close();
  assert_int_equal(rc, 0);
  assert_string_equal(updated_b.subscription, "from");

  /* A should have received a roster push showing subscription='to'. */
  assert_true(sink_contains(&a_sink, "subscription='to'"));

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  teardown_test_db();
}

/* Full mutual subscribe flow: both sides end up with subscription='both'. */
static void test_mutual_subscribe_results_in_both(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  write_sink_t a_sink = {.len = 0}, b_sink = {.len = 0};
  xmpp_session_t a_ctx, b_ctx;

  assert_int_equal(feed_to_online(&a_ctx), 0);
  xmpp_session_table_register(&a_ctx, sink_write, &a_sink);

  assert_int_equal(feed_contact_to_online(&b_ctx), 0);
  xmpp_session_table_register(&b_ctx, sink_write, &b_sink);

  /* A subscribes to B. */
  const char* sub_ab = "<presence type='subscribe' to='contactuser@localhost'/>";
  xmpp_feed(&a_ctx, sub_ab, strlen(sub_ab), mock_write, NULL);

  /* B accepts. */
  const char* accept_ba = "<presence type='subscribed' to='testuser@localhost'/>";
  xmpp_feed(&b_ctx, accept_ba, strlen(accept_ba), mock_write, NULL);

  /* B subscribes to A. */
  a_sink.len = 0;
  b_sink.len = 0;
  const char* sub_ba = "<presence type='subscribe' to='testuser@localhost'/>";
  xmpp_feed(&b_ctx, sub_ba, strlen(sub_ba), mock_write, NULL);

  /* A accepts. */
  const char* accept_ab = "<presence type='subscribed' to='contactuser@localhost'/>";
  xmpp_feed(&a_ctx, accept_ab, strlen(accept_ab), mock_write, NULL);

  /* Both rosters should now have subscription='both'. */
  storage_roster_item_t item_a, item_b;
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  assert_int_equal(storage_roster_get("testuser@localhost", "contactuser@localhost", &item_a), 0);
  assert_int_equal(storage_roster_get("contactuser@localhost", "testuser@localhost", &item_b), 0);
  storage_db_close();

  assert_string_equal(item_a.subscription, "both");
  assert_string_equal(item_b.subscription, "both");

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  teardown_test_db();
}

/* A unsubscribes: A loses 'to', B loses 'from'. */
static void test_unsubscribe_removes_to_direction(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Seed both sides with subscription='both'. */
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  storage_roster_item_t a_seed, b_seed;
  memset(&a_seed, 0, sizeof(a_seed));
  memset(&b_seed, 0, sizeof(b_seed));
  strncpy(a_seed.contact_jid, "contactuser@localhost", sizeof(a_seed.contact_jid) - 1);
  strncpy(a_seed.subscription, "both", sizeof(a_seed.subscription) - 1);
  strncpy(b_seed.contact_jid, "testuser@localhost", sizeof(b_seed.contact_jid) - 1);
  strncpy(b_seed.subscription, "both", sizeof(b_seed.subscription) - 1);
  storage_roster_upsert("testuser@localhost", &a_seed, NULL, 0);
  storage_roster_upsert("contactuser@localhost", &b_seed, NULL, 0);
  storage_db_close();

  write_sink_t b_sink = {.len = 0};
  xmpp_session_t a_ctx, b_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);
  assert_int_equal(feed_contact_to_online(&b_ctx), 0);
  xmpp_session_table_register(&b_ctx, sink_write, &b_sink);

  /* A sends unsubscribe to B. */
  const char* unsub = "<presence type='unsubscribe' to='contactuser@localhost'/>";
  xmpp_feed(&a_ctx, unsub, strlen(unsub), mock_write, NULL);

  /* B receives unsubscribe stanza. */
  assert_true(sink_contains(&b_sink, "type='unsubscribe'"));

  /* A's sub: both→from; B's sub: both→to. */
  storage_roster_item_t item_a, item_b;
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  assert_int_equal(storage_roster_get("testuser@localhost", "contactuser@localhost", &item_a), 0);
  assert_int_equal(storage_roster_get("contactuser@localhost", "testuser@localhost", &item_b), 0);
  storage_db_close();

  assert_string_equal(item_a.subscription, "from");
  assert_string_equal(item_b.subscription, "to");

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  teardown_test_db();
}

/* B refuses: unsubscribed → A's pending ask cleared. */
static void test_unsubscribed_clears_ask(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* A has pending ask for B. */
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  storage_roster_item_t a_seed;
  memset(&a_seed, 0, sizeof(a_seed));
  strncpy(a_seed.contact_jid, "contactuser@localhost", sizeof(a_seed.contact_jid) - 1);
  strncpy(a_seed.subscription, "none", sizeof(a_seed.subscription) - 1);
  a_seed.ask = 1;
  storage_roster_upsert("testuser@localhost", &a_seed, NULL, 0);
  storage_db_close();

  write_sink_t a_sink = {.len = 0};
  xmpp_session_t a_ctx, b_ctx;
  assert_int_equal(feed_to_online(&a_ctx), 0);
  xmpp_session_table_register(&a_ctx, sink_write, &a_sink);
  assert_int_equal(feed_contact_to_online(&b_ctx), 0);

  /* B rejects. */
  const char* refuse = "<presence type='unsubscribed' to='testuser@localhost'/>";
  xmpp_feed(&b_ctx, refuse, strlen(refuse), mock_write, NULL);

  /* A receives unsubscribed stanza. */
  assert_true(sink_contains(&a_sink, "type='unsubscribed'"));

  /* A's ask should be cleared. */
  storage_roster_item_t item_a;
  { sqlite3* _db; assert_int_equal(storage_db_open(&_db), 0); }
  int rc = storage_roster_get("testuser@localhost", "contactuser@localhost", &item_a);
  storage_db_close();
  assert_int_equal(rc, 0);
  assert_int_equal(item_a.ask, 0);
  assert_string_equal(item_a.subscription, "none");

  xmpp_session_cleanup(&a_ctx);
  xmpp_session_cleanup(&b_ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      /* Registry */
      cmocka_unit_test_setup_teardown(test_register_and_write, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_write_unknown_session_returns_minus1, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_unregister_removes_session, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_re_register_updates_callbacks, presence_test_setup, presence_test_teardown),

      /* Initial presence */
      cmocka_unit_test_setup_teardown(test_initial_presence_reaches_subscriber, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_initial_presence_not_sent_to_non_subscriber, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_initial_presence_sent_to_both_subscription, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_initial_presence_sent_to_own_other_resource, presence_test_setup, presence_test_teardown),

      /* Unavailable presence */
      cmocka_unit_test_setup_teardown(test_unavailable_presence_reaches_subscriber, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_unavailable_unregisters_session, presence_test_setup, presence_test_teardown),

      /* Directed presence */
      cmocka_unit_test_setup_teardown(test_directed_presence_delivered_to_full_jid, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_directed_presence_delivered_to_bare_jid, presence_test_setup, presence_test_teardown),

      /* Disconnect */
      cmocka_unit_test_setup_teardown(test_disconnect_broadcasts_unavailable, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_disconnect_noop_when_never_sent_presence, presence_test_setup, presence_test_teardown),

      /* Subscription handshake (RFC 6121 §3) */
      cmocka_unit_test_setup_teardown(test_subscribe_delivered_to_online_target, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_subscribed_updates_both_rosters, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_mutual_subscribe_results_in_both, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_unsubscribe_removes_to_direction, presence_test_setup, presence_test_teardown),
      cmocka_unit_test_setup_teardown(test_unsubscribed_clears_ask, presence_test_setup, presence_test_teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
