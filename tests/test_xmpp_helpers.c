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
#include "storage/users.h"
#include "test_xmpp_helpers.h"

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
  char path[512];
  snprintf(path, sizeof(path), "/tmp/test_xmpp_%d_%d.db", getpid(), (int)time(NULL));
  unlink(path);

  extern server_config_t server_config;
  strncpy(server_config.db_path, path, sizeof(server_config.db_path) - 1);
  server_config.db_path[sizeof(server_config.db_path) - 1] = '\0';

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

  storage_db_close();

  *db_path_out = server_config.db_path;
  return 0;
}

void teardown_test_db(void) {
  storage_db_close();
  extern server_config_t server_config;
  server_config.db_path[0] = '\0';
}

const char* buf_contains(const char* needle) {
  return memmem(g_write_buf, g_write_len, needle, strlen(needle));
}

void simulate_starttls(xmpp_session_t* ctx) {
  ctx->state = XMPP_STATE_TLS_NEGOTIATED;
  ctx->needs_parser_reset = 1;
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
