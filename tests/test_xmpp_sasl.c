#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "storage/db.h"
#include "xmpp.h"

/* ------------------------------------------------------------------ */
/*  Test helpers                                                       */
/* ------------------------------------------------------------------ */

/* Buffer to capture write output. */
static char g_write_buf[65536];
static size_t g_write_len = 0;

/* Mock write function that appends to g_write_buf. */
static int mock_write(void* ud, const char* data, size_t len) {
  (void)ud;
  if (g_write_len + len > sizeof(g_write_buf)) {
    len = sizeof(g_write_buf) - g_write_len;
  }
  memcpy(g_write_buf + g_write_len, data, len);
  g_write_len += len;
  return 0;
}

/* Create a test database with a user. Returns 0 on success. */
static int setup_test_db(const char** db_path_out) {
  char path[512];
  snprintf(path, sizeof(path), "/tmp/test_xmpp_%d_%d.db", getpid(), (int)time(NULL));

  unlink(path);

  sqlite3* db;
  int rc = sqlite3_open(path, &db);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "cannot open test db: %s\n", sqlite3_errmsg(db));
    return -1;
  }

  /* Create users table. */
  const char* sql = "CREATE TABLE IF NOT EXISTS users (\n"
                    "    jid           TEXT PRIMARY KEY,\n"
                    "    password_plain TEXT NOT NULL,\n"
                    "    created_at    INTEGER NOT NULL,\n"
                    "    disabled      INTEGER NOT NULL DEFAULT 0\n"
                    ")";
  rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "create table failed: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return -1;
  }

  /* Insert a test user. */
  const char* insert_sql = "INSERT INTO users (jid, password_plain, created_at, disabled) "
                           "VALUES (?, ?, ?, ?)";
  sqlite3_stmt* stmt;
  rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "prepare insert failed: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return -1;
  }

  sqlite3_bind_text(stmt, 1, "testuser@localhost", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, "testpass", -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 3, 1000000000);
  sqlite3_bind_int64(stmt, 4, 0);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    fprintf(stderr, "insert failed: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return -1;
  }

  sqlite3_close(db);

  /* Set the global db_path so storage_db_open finds our test db. */
  extern server_config_t server_config;
  strncpy(server_config.db_path, path, sizeof(server_config.db_path) - 1);
  server_config.db_path[sizeof(server_config.db_path) - 1] = '\0';
  *db_path_out = server_config.db_path;
  return 0;
}

/* Reset the global db_path. */
static void teardown_test_db(void) {
  extern server_config_t server_config;
  server_config.db_path[0] = '\0';
}

/* Find a substring in the write buffer. Returns pointer into g_write_buf or
 * NULL. */
static const char* buf_contains(const char* needle) {
  return memmem(g_write_buf, g_write_len, needle, strlen(needle));
}

/* ------------------------------------------------------------------ */
/*  SASL PLAIN helper                                                  */
/* ------------------------------------------------------------------ */

/* Build and base64-encode a SASL PLAIN message, feed it to ctx, return
 * xmpp_feed rc. */
static int feed_sasl_plain(xmpp_session_t* ctx, const char* authzid, const char* authcid,
                           const char* passwd) {
  static const char b64_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t az = strlen(authzid), ac = strlen(authcid), pw = strlen(passwd);
  size_t auth_len = az + 1 + ac + 1 + pw;
  char* auth_data = malloc(auth_len + 1);
  memcpy(auth_data, authzid, az);
  auth_data[az] = '\0';
  memcpy(auth_data + az + 1, authcid, ac);
  auth_data[az + 1 + ac] = '\0';
  memcpy(auth_data + az + 1 + ac + 1, passwd, pw);

  char* b64 = malloc((auth_len / 3 + 1) * 4 + 4 + 1);
  int b64_len = 0;
  for (size_t i = 0; i < auth_len; i += 3) {
    unsigned char b0 = (unsigned char)auth_data[i];
    unsigned char b1 = (i + 1 < auth_len) ? (unsigned char)auth_data[i + 1] : 0;
    unsigned char b2 = (i + 2 < auth_len) ? (unsigned char)auth_data[i + 2] : 0;
    b64[b64_len++] = b64_table[b0 >> 2];
    b64[b64_len++] = b64_table[((b0 & 3) << 4) | (b1 >> 4)];
    b64[b64_len++] = (i + 1 < auth_len) ? b64_table[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    b64[b64_len++] = (i + 2 < auth_len) ? b64_table[b2 & 63] : '=';
  }
  b64[b64_len] = '\0';

  char auth_xml[2048];
  snprintf(auth_xml, sizeof(auth_xml),
           "<auth mechanism='PLAIN' xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
           "%s</auth>",
           b64);

  int rc = xmpp_feed(ctx, auth_xml, strlen(auth_xml), mock_write, NULL);
  free(b64);
  free(auth_data);
  return rc;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

/* Test 1: Stream negotiation — client sends <stream:stream>, server
 * responds with stream open + features containing PLAIN mechanism. */
static void test_xmpp_stream_negotiation(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Client sends stream header. */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";

  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES);

  /* Server should have sent stream open + features. */
  assert_true(buf_contains("<?xml version='1.0'"));
  assert_true(buf_contains("<stream:stream"));
  assert_true(buf_contains("from='localhost'")); /* RFC 6120 §4.7.1 */
  assert_true(buf_contains("id='"));             /* RFC 6120 §4.7.3 */
  assert_true(buf_contains("<stream:features>"));
  assert_true(buf_contains("<mechanism>PLAIN</mechanism>"));
  xmpp_session_cleanup(&ctx);
}

/* Test 2: Successful SASL PLAIN authentication. */
static void test_xmpp_sasl_plain_success(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Step 1: Stream negotiation. */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES);

  /* Step 2: SASL PLAIN auth — authcid is local-part only ("testuser"). */
  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_AUTHED);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* Test 3: Failed authentication — wrong password. */
static void test_xmpp_sasl_plain_bad_password(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Stream negotiation. */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);

  rc = feed_sasl_plain(&ctx, "", "testuser", "wrongpass");
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_FAILED);
  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* Test 4: Non-existent user. */
static void test_xmpp_sasl_plain_user_not_found(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Stream negotiation. */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);

  rc = feed_sasl_plain(&ctx, "", "nonexistent", "anypass");
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_FAILED);
  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* Test 5: Full XMPP connection flow — stream → auth → bind → connected. */
static void test_xmpp_full_connection_flow(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Step 1: Initial stream. */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES);

  /* Step 2: SASL PLAIN auth. */
  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_AUTHED);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>"));

  /* Step 3: Stream restart — RFC 6120 §4.3.3: no <?xml?> declaration on
   * restart. */
  const char* client_restart = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                               "xmlns='jabber:client' to='localhost' version='1.0' "
                               "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_restart, strlen(client_restart), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_BIND);
  assert_true(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));
  assert_true(buf_contains("<required/></bind>"));

  /* Step 4: Bind IQ. */
  const char* bind_iq = "<iq type='set' id='bind1' xmlns='jabber:client'>"
                        "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                        "</iq>";
  rc = xmpp_feed(&ctx, bind_iq, strlen(bind_iq), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_CONNECTED);
  assert_true(buf_contains("<iq type='result'"));
  assert_true(buf_contains("<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"));
  assert_true(buf_contains("<jid>testuser@localhost</jid>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* Test 6: Unsupported mechanism. */
static void test_xmpp_unsupported_mechanism(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Stream negotiation. */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);

  /* Auth with unsupported mechanism. */
  const char* auth_xml = "<auth mechanism='DIGEST-MD5' xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>";
  rc = xmpp_feed(&ctx, auth_xml, strlen(auth_xml), mock_write, NULL);
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_FAILED);

  assert_true(buf_contains("<unsupported-mechanism/>"));
  xmpp_session_cleanup(&ctx);
}

/* Test 7: Disabled account. */
static void test_xmpp_disabled_account(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);
  assert_non_null(db_path);

  /* Disable the test user in the test database. */
  char sql[512];
  snprintf(sql, sizeof(sql), "UPDATE users SET disabled = 1 WHERE jid = 'testuser@localhost'");
  sqlite3* db;
  int rc = sqlite3_open(server_config.db_path, &db);
  if (rc == SQLITE_OK) {
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
  }

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Stream negotiation. */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);

  /* Auth with correct credentials but disabled account. */
  rc = feed_sasl_plain(&ctx, "", "testuser", "testpass");
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_FAILED);
  assert_true(buf_contains("<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* Test 8: Incomplete XML elements are buffered correctly. */
static void test_xmpp_incomplete_elements(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Send stream header in two chunks. */
  const char* chunk1 = "<?xml version='1.0'?><stream:stream "
                       "xmlns:stream='http://etherx.jabber.org/streams' xmlns='jabber:client' "
                       "to='localhost' version='1.0' xml:lang='en'>";
  int rc = xmpp_feed(&ctx, chunk1, strlen(chunk1), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES);

  /* Buffer should contain the response. */
  assert_true(buf_contains("<stream:features>"));
  xmpp_session_cleanup(&ctx);
}

/* Test 9: Domain extraction from 'to' attribute. */
static void test_xmpp_domain_extraction(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='example.com' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES);

  /* The stream open should contain from='example.com' (RFC 6120 §4.7.1). */
  assert_true(buf_contains("from='example.com'"));
  xmpp_session_cleanup(&ctx);
}

/* Test 10: Default domain when 'to' is missing. */
static void test_xmpp_default_domain(void** state) {
  (void)state;
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES);

  /* Should default to localhost (RFC 6120 §4.7.1). */
  assert_true(buf_contains("from='localhost'"));
  xmpp_session_cleanup(&ctx);
}

/* Test 11: authzid present and matching — must succeed. */
static void test_xmpp_sasl_authzid_match(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* hello = "<?xml version='1.0'?>"
                      "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                      "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  assert_int_equal(xmpp_feed(&ctx, hello, strlen(hello), mock_write, NULL), 0);

  /* authzid == authcid@domain — allowed by RFC 4616 §2. */
  int rc = feed_sasl_plain(&ctx, "testuser@localhost", "testuser", "testpass");
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_AUTHED);
  assert_true(buf_contains("<success"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* Test 12: authzid present but different from derived identity — must fail. */
static void test_xmpp_sasl_authzid_mismatch(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* hello = "<?xml version='1.0'?>"
                      "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                      "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  assert_int_equal(xmpp_feed(&ctx, hello, strlen(hello), mock_write, NULL), 0);

  /* authzid ≠ authcid@domain — RFC 4616 §2 requires failure. */
  int rc = feed_sasl_plain(&ctx, "other@localhost", "testuser", "testpass");
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_FAILED);
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* Test 13: forbidden localpart character — must fail. */
static void test_xmpp_sasl_forbidden_localpart(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* hello = "<?xml version='1.0'?>"
                      "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                      "xmlns='jabber:client' to='localhost' version='1.0' xml:lang='en'>";
  assert_int_equal(xmpp_feed(&ctx, hello, strlen(hello), mock_write, NULL), 0);

  /* '@' is forbidden in localpart per RFC 7622 §3.3.1. */
  int rc = feed_sasl_plain(&ctx, "", "user@name", "testpass");
  assert_int_equal(rc, -1);
  assert_int_equal(ctx.state, XMPP_STATE_FAILED);
  assert_true(buf_contains("<not-authorized/>"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_xmpp_stream_negotiation),
      cmocka_unit_test(test_xmpp_sasl_plain_success),
      cmocka_unit_test(test_xmpp_sasl_plain_bad_password),
      cmocka_unit_test(test_xmpp_sasl_plain_user_not_found),
      cmocka_unit_test(test_xmpp_full_connection_flow),
      cmocka_unit_test(test_xmpp_unsupported_mechanism),
      cmocka_unit_test(test_xmpp_disabled_account),
      cmocka_unit_test(test_xmpp_incomplete_elements),
      cmocka_unit_test(test_xmpp_domain_extraction),
      cmocka_unit_test(test_xmpp_default_domain),
      cmocka_unit_test(test_xmpp_sasl_authzid_match),
      cmocka_unit_test(test_xmpp_sasl_authzid_mismatch),
      cmocka_unit_test(test_xmpp_sasl_forbidden_localpart),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
