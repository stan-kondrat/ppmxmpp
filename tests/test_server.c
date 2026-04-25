#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <uv.h>

#include "config.h"
#include "server.h"

static int setup_test_config(const char* host, int port) {
  char conf_path[512];
  char db_path[512];
  snprintf(conf_path, sizeof(conf_path), "/tmp/test_server_conf_%d_%d_%d.conf", getpid(),
           (int)time(NULL), rand());
  snprintf(db_path, sizeof(db_path), "/tmp/test_server_%d_%d.db", getpid(), (int)time(NULL));

  FILE* f = fopen(conf_path, "w");
  if (!f) {
    return -1;
  }
  fprintf(f, "db_path = \"%s\";\n", db_path);
  fprintf(f, "log_level = \"ERROR\";\n");
  fprintf(f, "bind_host = \"%s\";\n", host);
  fprintf(f, "bind_port = %d;\n", port);
  fclose(f);

  server_config = config_parse_default_config();
  int rc = config_load(conf_path);
  unlink(conf_path);
  return rc;
}

/* Find a free TCP port by binding to port 0 and reading back the assigned port.
 */
static int find_free_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, (struct sockaddr*)&addr, &len);
  int port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

/* Close all active handles on a loop so uv_run() can drain to completion. */
static void close_all_cb(uv_handle_t* handle, void* arg) {
  (void)arg;
  if (!uv_is_closing(handle)) {
    uv_close(handle, NULL);
  }
}

static void drain_loop(uv_loop_t* loop) {
  uv_walk(loop, close_all_cb, NULL);
  uv_run(loop, UV_RUN_DEFAULT);
  uv_loop_close(loop);
}

/* server_start() with a valid address should return 0 and listen. */
static void test_server_start_success(void** state) {
  (void)state;
  int port = find_free_port();
  assert_true(port > 0);
  assert_int_equal(setup_test_config("127.0.0.1", port), 0);
  uv_loop_t loop;
  uv_loop_init(&loop);
  assert_int_equal(server_start(&loop), 0);
  drain_loop(&loop);
}

/* server_start() with a non-IP host string should return -1 (uv_ip4_addr
 * fails). */
static void test_server_start_invalid_host(void** state) {
  (void)state;
  assert_int_equal(setup_test_config("not.a.valid.ip", 5222), 0);
  uv_loop_t loop;
  uv_loop_init(&loop);
  assert_int_equal(server_start(&loop), -1);
  drain_loop(&loop);
}

/* server_start() when the port is already in use should return -1. */
static void test_server_start_port_in_use(void** state) {
  (void)state;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert_true(fd >= 0);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  assert_int_equal(bind(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
  socklen_t slen = sizeof(addr);
  getsockname(fd, (struct sockaddr*)&addr, &slen);
  int port = ntohs(addr.sin_port);
  assert_int_equal(listen(fd, 1), 0);

  assert_int_equal(setup_test_config("127.0.0.1", port), 0);
  uv_loop_t loop;
  uv_loop_init(&loop);
  assert_int_equal(server_start(&loop), -1);
  drain_loop(&loop);
  close(fd);
}

int main(void) {
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_server_start_success),
      cmocka_unit_test(test_server_start_invalid_host),
      cmocka_unit_test(test_server_start_port_in_use),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
