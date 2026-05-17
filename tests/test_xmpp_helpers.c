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
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "server.h"
#include "storage/db.h"
#include "storage/db_users.h"
#include "test_xmpp_helpers.h"
#include "xmpp.h"

char g_write_buf[65536];
size_t g_write_len = 0;

int mock_write(void* ud, const char* data, size_t len) {
  (void)ud;
  if (g_write_len + len > sizeof(g_write_buf)) {
    len = sizeof(g_write_buf) - g_write_len;
  }
  memcpy(g_write_buf + g_write_len, data, len);
  g_write_len += len;
  return 0;
}

int setup_test_db(const char** db_path_out) {
  extern server_config_t server_config;

  const char* tmpdir = getenv("TMPDIR");
  if (!tmpdir || tmpdir[0] == '\0') tmpdir = P_tmpdir;

  char path[512];
  snprintf(path, sizeof(path), "%s/test_xmpp_XXXXXX", tmpdir);
  int fd = mkstemp(path);
  if (fd < 0) {
    fprintf(stderr, "setup_test_db: mkstemp failed\n");
    return -1;
  }
  close(fd);
  unlink(path);  /* SQLite creates the file itself */

  snprintf(server_config.db_path, sizeof(server_config.db_path), "%s", path);

  /* Initialize handlers once per test process. db_path must be set first so
   * server_init's storage_db_open succeeds. */
  static int handlers_initialized = 0;
  if (!handlers_initialized) {
    log_init();
    log_silence();
    if (server_init() != 0) {
      fprintf(stderr, "setup_test_db: server_init failed\n");
      return -1;
    }
    storage_db_close();  /* re-open below with fresh per-test path */
    handlers_initialized = 1;
  }

  /* Open via the migration system so all schema versions are applied. */
  sqlite3* db;
  if (storage_db_open(&db) != 0) {
    fprintf(stderr, "setup_test_db: storage_db_open failed\n");
    return -1;
  }

  if (storage_users_create("testuser@localhost", "testpass") != 0) {
    fprintf(stderr, "setup_test_db: create user failed\n");
    storage_db_close();
    return -1;
  }

  /* Also seed testuser@example.com used by some state tests. */
  (void)storage_users_create("testuser@example.com", "testpass");
  /* Seed bob@localhost used by carbons and message routing tests. */
  (void)storage_users_create("bob@localhost", "testpass");
  /* Seed alice@localhost used by XEP-0245 and other message routing tests. */
  (void)storage_users_create("alice@localhost", "testpass");

  storage_db_close();

  *db_path_out = server_config.db_path;
  return 0;
}

void teardown_test_db(void) {
  /* Capture path before clearing server_config.db_path, then delete the
   * file so the next setup_test_db starts with a truly empty database. */
  extern server_config_t server_config;
  char path[sizeof(server_config.db_path)];
  (void)snprintf(path, sizeof(path), "%s", server_config.db_path);
  storage_db_close();
  server_config.db_path[0] = '\0';
  if (path[0] != '\0') {
    (void)unlink(path);
  }
}

const char* buf_contains(const char* needle) {
  return memmem(g_write_buf, g_write_len, needle, strlen(needle));
}

void reset_write_buf(void) {
  g_write_len = 0;
}

void simulate_starttls(xmpp_session_t* ctx) {
  ctx->state = XMPP_STATE_TLS_NEGOTIATED;
  ctx->needs_parser_reset = 1;
}

int feed_to_online_as(xmpp_session_t* ctx, const char* username, const char* passwd) {
  memset(ctx, 0, sizeof(*ctx));
  xmpp_session_reset(ctx);
  g_write_len = 0;

  char buf[512];
  snprintf(buf, sizeof(buf),
           "<?xml version='1.0'?>"
           "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
           " xmlns='jabber:client' to='localhost' version='1.0'>");
  if (xmpp_feed(ctx, buf, strlen(buf), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_FEATURES_RECEIVED) return -1;

  g_write_len = 0;
  const char* starttls = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>";
  if (xmpp_feed(ctx, starttls, strlen(starttls), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_STARTTLS_SENT) return -1;

  g_write_len = 0;
  const char* r1 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  if (xmpp_feed(ctx, r1, strlen(r1), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_FEATURES_RECEIVED_POST_TLS) return -1;

  if (feed_sasl_plain(ctx, "", username, passwd) != 0) return -1;
  if (ctx->state != XMPP_STATE_SASL_SUCCESS) return -1;

  g_write_len = 0;
  const char* r2 = "<stream:stream xmlns:stream='http://etherx.jabber.org/streams'"
                   " xmlns='jabber:client' to='localhost' version='1.0'>";
  if (xmpp_feed(ctx, r2, strlen(r2), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_BOUND) return -1;

  g_write_len = 0;
  const char* bind = "<iq type='set' id='b1'>"
                     "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                     "<resource>test</resource></bind></iq>";
  if (xmpp_feed(ctx, bind, strlen(bind), mock_write, NULL) != 0) return -1;
  if (ctx->state != XMPP_STATE_ONLINE) return -1;

  g_write_len = 0;
  return 0;
}

int feed_to_online(xmpp_session_t* ctx) {
  return feed_to_online_as(ctx, "testuser", "testpass");
}

int feed_sasl_plain(xmpp_session_t* ctx, const char* authzid, const char* authcid,
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
