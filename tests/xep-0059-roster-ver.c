#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "storage/db.h"
#include "storage/db_roster.h"
#include "test_xmpp_helpers.h"
#include "xep-0059-roster-ver.h"
#include "xmpp.h"

/* feed_to_online() is provided by test_xmpp_helpers.c. */

/* ------------------------------------------------------------------ */
/*  1. SHA-256 computation produces 64-char hex                       */
/* ------------------------------------------------------------------ */

static void test_sha256_produces_64hex(void** state) {
  (void)state;
  char ver[ROSTER_VER_SIZE];
  int rc = roster_ver_compute_sha256("test", 4, ver);
  assert_int_equal(rc, 0);
  assert_int_equal(strlen(ver), 64);
  /* SHA-256 of "test" = 9f86d08... */
  assert_string_equal(ver, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
}

/* ------------------------------------------------------------------ */
/*  2. Identical input strings -> identical hashes                     */
/* ------------------------------------------------------------------ */

static void test_sha256_deterministic(void** state) {
  (void)state;
  char v1[ROSTER_VER_SIZE];
  char v2[ROSTER_VER_SIZE];
  assert_int_equal(roster_ver_compute_sha256("hello", 5, v1), 0);
  assert_int_equal(roster_ver_compute_sha256("hello", 5, v2), 0);
  assert_string_equal(v1, v2);
}

/* ------------------------------------------------------------------ */
/*  3. Different input strings -> different hashes                    */
/* ------------------------------------------------------------------ */

static void test_sha256_different_inputs(void** state) {
  (void)state;
  char v1[ROSTER_VER_SIZE];
  char v2[ROSTER_VER_SIZE];
  assert_int_equal(roster_ver_compute_sha256("a", 1, v1), 0);
  assert_int_equal(roster_ver_compute_sha256("b", 1, v2), 0);
  assert_false(strcmp(v1, v2) == 0);
}

/* ------------------------------------------------------------------ */
/*  4. Canonical string accumulation and finalisation                  */
/* ------------------------------------------------------------------ */

static void test_append_and_finalise(void** state) {
  (void)state;
  char buf[1024];
  roster_ver_ctx_t ctx = {buf, 0, sizeof(buf), 0};

  const char* groups1[] = {"Friends", "Work"};
  roster_ver_append_item(&ctx, "alice@example.com", "Alice", "both", groups1, 2);
  assert_int_equal(ctx.error, 0);

  const char* groups2[] = {"Work"};
  roster_ver_append_item(&ctx, "bob@example.com", "", "none", groups2, 1);
  assert_int_equal(ctx.error, 0);

  assert_int_equal(roster_ver_finalise(&ctx), 0);
  assert_true(ctx.len > 0);
  assert_int_equal(ctx.buf[ctx.len], '\0');

  /* First item */
  assert_non_null(strstr(buf, "alice@example.com"));
  assert_non_null(strstr(buf, "both"));
  assert_non_null(strstr(buf, "Alice"));
  /* Second item */
  assert_non_null(strstr(buf, "bob@example.com"));
  assert_non_null(strstr(buf, "none"));
  /* Items separated by '|' */
  assert_non_null(strchr(buf, '|'));
}

/* ------------------------------------------------------------------ */
/*  5. Increment and get roundtrip after roster upsert                 */
/* ------------------------------------------------------------------ */

static void test_increment_get_roundtrip(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* No version yet. */
  char v0[ROSTER_VER_SIZE];
  int rc = roster_ver_get("testuser@localhost", v0);
  assert_int_equal(rc, 1); /* not found */

  /* Add a contact — that bumps the version. */
  storage_roster_item_t item;
  memset(&item, 0, sizeof(item));
  strncpy(item.contact_jid, "alice@example.com", sizeof(item.contact_jid) - 1);
  strncpy(item.name, "Alice", sizeof(item.name) - 1);
  strncpy(item.subscription, "both", sizeof(item.subscription) - 1);
  const char* groups[] = {"Friends"};
  assert_int_equal(storage_roster_upsert("testuser@localhost", &item, groups, 1), 0);
  storage_db_close();

  /* Now a version exists. */
  char v1[ROSTER_VER_SIZE];
  rc = roster_ver_get("testuser@localhost", v1);
  assert_int_equal(rc, 0);
  assert_int_equal(strlen(v1), 64);

  /* Add another contact — version changes. */
  memset(&item, 0, sizeof(item));
  strncpy(item.contact_jid, "bob@example.com", sizeof(item.contact_jid) - 1);
  strncpy(item.subscription, "none", sizeof(item.subscription) - 1);
  assert_int_equal(storage_roster_upsert("testuser@localhost", &item, NULL, 0), 0);
  storage_db_close();

  char v2[ROSTER_VER_SIZE];
  rc = roster_ver_get("testuser@localhost", v2);
  assert_int_equal(rc, 0);
  assert_false(strcmp(v1, v2) == 0);

  /* Remove bob — version changes again. */
  assert_int_equal(storage_roster_remove("testuser@localhost", "bob@example.com"), 0);
  storage_db_close();

  char v3[ROSTER_VER_SIZE];
  rc = roster_ver_get("testuser@localhost", v3);
  assert_int_equal(rc, 0);
  assert_false(strcmp(v2, v3) == 0);

  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  6. Roster get without ver -> result has ver=                       */
/* ------------------------------------------------------------------ */

static void test_roster_get_without_ver_has_ver_attr(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  g_write_len = 0;
  const char* iq = "<iq type='get' id='rv1'>"
                  "<query xmlns='jabber:iq:roster'/></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='result' id='rv1'>"));
  assert_true(buf_contains("ver='"));
  assert_true(buf_contains("query xmlns='jabber:iq:roster'"));
  assert_true(buf_contains("</query>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  7. Roster get with matching ver -> empty result with ver=         */
/* ------------------------------------------------------------------ */

static void test_roster_get_with_matching_ver(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Seed one contact so version is stable. */
  storage_roster_item_t item;
  memset(&item, 0, sizeof(item));
  strncpy(item.contact_jid, "alice@example.com", sizeof(item.contact_jid) - 1);
  strncpy(item.subscription, "both", sizeof(item.subscription) - 1);
  assert_int_equal(storage_roster_upsert("testuser@localhost", &item, NULL, 0), 0);
  storage_db_close();

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  /* Get the current version from the DB. */
  char stored_ver[ROSTER_VER_SIZE];
  (void)roster_ver_get("testuser@localhost", stored_ver);

  g_write_len = 0;
  char iq[512];
  snprintf(iq, sizeof(iq),
           "<iq type='get' id='rv2'>"
           "<query xmlns='jabber:iq:roster' ver='%s'/></iq>",
           stored_ver);
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  /* Result IQ with ver= and empty query (no <item> elements). */
  assert_true(buf_contains("<iq type='result' id='rv2'>"));
  assert_true(buf_contains("ver='"));
  /* No items because roster is unchanged. */
  assert_false(buf_contains("<item jid='alice@example.com'"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  8. Roster get with mismatching ver -> full roster + new ver       */
/* ------------------------------------------------------------------ */

static void test_roster_get_with_mismatch_ver(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* Seed one contact so version is stable. */
  storage_roster_item_t item;
  memset(&item, 0, sizeof(item));
  strncpy(item.contact_jid, "bob@example.com", sizeof(item.contact_jid) - 1);
  strncpy(item.name, "Bob", sizeof(item.name) - 1);
  strncpy(item.subscription, "both", sizeof(item.subscription) - 1);
  const char* groups[] = {"Friends"};
  assert_int_equal(storage_roster_upsert("testuser@localhost", &item, groups, 1), 0);
  storage_db_close();

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  g_write_len = 0;
  /* Send a deliberately wrong version. */
  const char* iq = "<iq type='get' id='rv3'>"
                  "<query xmlns='jabber:iq:roster' ver='this-is-not-a-real-version'/>"
                  "</iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_true(buf_contains("<iq type='result' id='rv3'>"));
  assert_true(buf_contains("ver='"));
  /* Full roster content returned because version mismatched. */
  assert_true(buf_contains("bob@example.com"));
  assert_true(buf_contains("Bob"));
  assert_true(buf_contains("subscription='both'"));
  assert_true(buf_contains("<group>Friends</group>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  9. Roster set triggers push with ver=                             */
/* ------------------------------------------------------------------ */

static void test_roster_set_push_has_ver_attr(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  assert_int_equal(feed_to_online(&ctx), 0);

  g_write_len = 0;
  const char* iq = "<iq type='set' id='rvs1'>"
                  "<query xmlns='jabber:iq:roster'>"
                  "<item jid='carol@example.com' name='Carol'/>"
                  "</query></iq>";
  int rc = xmpp_feed(&ctx, iq, strlen(iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  /* Roster push (type='set', id='push-...') includes ver= */
  assert_true(buf_contains("<iq type='set' id='push-rvs1'>"));
  assert_true(buf_contains("jabber:iq:roster'"));
  assert_true(buf_contains("ver='"));
  assert_true(buf_contains("carol@example.com"));
  assert_true(buf_contains("Carol"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_sha256_produces_64hex),
      cmocka_unit_test(test_sha256_deterministic),
      cmocka_unit_test(test_sha256_different_inputs),
      cmocka_unit_test(test_append_and_finalise),
      cmocka_unit_test(test_increment_get_roundtrip),
      cmocka_unit_test(test_roster_get_without_ver_has_ver_attr),
      cmocka_unit_test(test_roster_get_with_matching_ver),
      cmocka_unit_test(test_roster_get_with_mismatch_ver),
      cmocka_unit_test(test_roster_set_push_has_ver_attr),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}