#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "config.h"
#include "log.h"
#include "server.h"
#include "tls.h"
#include "xmpp.h"

#include <sys/stat.h>

#define READ_BUF_SIZE 65536

/* Per-connection state.  client must be first so (conn_t *) == (uv_handle_t *).
 */
typedef struct {
  uv_tcp_t client;
  char buf[READ_BUF_SIZE];
  uint64_t id;
  xmpp_session_t xmpp;
} conn_t;

/* Write request that owns its payload until write_cb fires. */
typedef struct {
  uv_write_t req;
  uv_buf_t buf;
  char data[]; /* payload copied here; freed together with req */
} write_req_t;

static uv_tcp_t g_server;
static uv_tcp_t g_tls_server;
static uv_signal_t g_sigint;
static uv_signal_t g_sigterm;
static _Atomic uint64_t g_next_id = 1;

/* ------------------------------------------------------------------ close */

static void on_conn_close(uv_handle_t* handle) {
  conn_t* conn = (conn_t*)handle;
  xmpp_session_cleanup(&conn->xmpp);
  stump_d("conn %" PRIu64 ": connection freed", conn->id);
  free(conn);
}

static void close_conn(conn_t* conn) {
  if (!uv_is_closing((uv_handle_t*)&conn->client)) {
    uv_close((uv_handle_t*)&conn->client, on_conn_close);
  }
}

/* ------------------------------------------------------------------ alloc */

static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  (void)suggested_size;
  conn_t* conn = (conn_t*)handle;
  buf->base = conn->buf;
  buf->len = sizeof(conn->buf);
}

/* ------------------------------------------------------------------ write */

static void write_cb(uv_write_t* req, int status) {
  write_req_t* wr = (write_req_t*)req;
  if (status < 0) {
    stump_er("write error: %s", uv_strerror(status));
  }
  free(wr);
}

/* Send len bytes to conn via libuv async write.
 * Data is copied into the request struct and remains valid until write_cb
 * fires. */
static int conn_write(void* ud, const char* data, size_t len) {
  conn_t* conn = (conn_t*)ud;
  write_req_t* wr = malloc(sizeof(*wr) + len);
  if (!wr) {
    stump_er("conn %" PRIu64 ": write alloc failed", conn->id);
    return -1;
  }
  memcpy(wr->data, data, len);
  wr->buf = uv_buf_init(wr->data, (unsigned int)len);
  int r = uv_write(&wr->req, (uv_stream_t*)&conn->client, &wr->buf, 1, write_cb);
  if (r < 0) {
    stump_er("conn %" PRIu64 ": uv_write: %s", conn->id, uv_strerror(r));
    free(wr);
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ read */

static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  conn_t* conn = (conn_t*)stream;

  if (nread < 0) {
    if (nread != UV_EOF) {
      stump_er("conn %" PRIu64 ": read error: %s", conn->id, uv_strerror((int)nread));
    }
    close_conn(conn);
    return;
  }

  if (nread == 0) {
    return;
  }

  /* Feed data into the XMPP state machine. */
  int rc = xmpp_feed(&conn->xmpp, buf->base, (size_t)nread, conn_write, conn);
  if (rc != 0) {
    /* XMPP state machine requests connection close. */
    stump_d("conn %" PRIu64 ": XMPP requested close", conn->id);
    close_conn(conn);
    return;
  }

  /* Log up to 64 bytes as hex; note truncation if buffer was larger. */
  size_t show = (size_t)nread > 64 ? 64 : (size_t)nread;
  char hex[64 * 3 + 1];
  for (size_t i = 0; i < show; i++) {
    snprintf(hex + i * 3, 4, "%02x ", (unsigned char)buf->base[i]);
  }
  hex[show * 3] = '\0';

  stump_d("conn %" PRIu64 ": recv %ld bytes%s: %s", conn->id, (long)nread,
          (size_t)nread > 64 ? " (first 64 shown)" : "", hex);
}

/* ------------------------------------------------------------------ accept */

static void on_new_connection(uv_stream_t* server, int status) {
  if (status < 0) {
    stump_er("accept error: %s", uv_strerror(status));
    return;
  }

  conn_t* conn = calloc(1, sizeof(conn_t));
  if (!conn) {
    stump_er("out of memory for new connection");
    return;
  }

  conn->id = atomic_fetch_add_explicit(&g_next_id, 1, memory_order_relaxed);
  uv_tcp_init(server->loop, &conn->client);
  /* Non-NULL data marks this handle as a conn_t in close_walk_cb. */
  conn->client.data = conn;
  xmpp_session_reset(&conn->xmpp);

  if (uv_accept(server, (uv_stream_t*)&conn->client) != 0) {
    stump_er("conn %" PRIu64 ": uv_accept failed", conn->id);
    uv_close((uv_handle_t*)&conn->client, on_conn_close);
    return;
  }

  stump_i("conn %" PRIu64 ": accepted", conn->id);

  if (uv_read_start((uv_stream_t*)&conn->client, alloc_cb, read_cb) != 0) {
    stump_er("conn %" PRIu64 ": uv_read_start failed", conn->id);
    close_conn(conn);
  }
}

/* ------------------------------------------------------------------ shutdown
 */

static void close_walk_cb(uv_handle_t* handle, void* arg) {
  (void)arg;
  if (!uv_is_closing(handle)) {
    /* handle->data is non-NULL only for conn_t handles (set in
     * on_new_connection). Server and signal handles leave data NULL, so they
     * get a no-op callback. */
    uv_close(handle, handle->data ? on_conn_close : NULL);
  }
}

static void on_signal(uv_signal_t* handle, int signum) {
  stump_i("signal %d received, shutting down", signum);
  uv_walk(handle->loop, close_walk_cb, NULL);
}

/* ------------------------------------------------------------------ start */

int file_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

int server_start(uv_loop_t* loop) {
  if (!server_config.bind_enabled && !server_config.tls_enabled) {
    stump_i("both bind and tls disabled, nothing to listen on");
    return 0;
  }

  if (server_config.bind_enabled) {
    struct sockaddr_in addr;
    int r;

    r = uv_ip4_addr(server_config.bind_host, server_config.bind_port, &addr);
    if (r != 0) {
      stump_er("invalid listen address %s:%d: %s", server_config.bind_host, server_config.bind_port,
               uv_strerror(r));
      return -1;
    }

    uv_tcp_init(loop, &g_server);
    /* g_server.data stays NULL — see close_walk_cb */

    r = uv_tcp_bind(&g_server, (const struct sockaddr*)&addr, 0);
    if (r != 0) {
      stump_er("uv_tcp_bind %s:%d: %s", server_config.bind_host, server_config.bind_port,
               uv_strerror(r));
      uv_close((uv_handle_t*)&g_server, NULL);
      return -1;
    }

    r = uv_listen((uv_stream_t*)&g_server, 128, on_new_connection);
    if (r != 0) {
      stump_er("uv_listen: %s", uv_strerror(r));
      uv_close((uv_handle_t*)&g_server, NULL);
      return -1;
    }

    stump_i("listening on %s:%d", server_config.bind_host, server_config.bind_port);
  }

  if (server_config.tls_enabled) {
    if (!file_exists(server_config.tls_cert_file) || !file_exists(server_config.tls_key_file)) {
      stump_i("TLS enabled but cert/key missing, generating self-signed "
              "certificate");
      if (generate_self_signed_cert(server_config.tls_cert_file, server_config.tls_key_file) != 0) {
        stump_er("failed to generate self-signed certificate");
        return -1;
      }
    }

    struct sockaddr_in tls_addr;
    int tls_r = uv_ip4_addr(server_config.tls_host, server_config.tls_port, &tls_addr);
    if (tls_r != 0) {
      stump_er("invalid TLS address %s:%d: %s", server_config.tls_host, server_config.tls_port,
               uv_strerror(tls_r));
      if (server_config.bind_enabled) {
        uv_close((uv_handle_t*)&g_server, NULL);
      }
      return -1;
    }

    uv_tcp_init(loop, &g_tls_server);
    g_tls_server.data = NULL;

    tls_r = uv_tcp_bind(&g_tls_server, (const struct sockaddr*)&tls_addr, 0);
    if (tls_r != 0) {
      stump_er("uv_tcp_bind %s:%d: %s", server_config.tls_host, server_config.tls_port,
               uv_strerror(tls_r));
      if (server_config.bind_enabled) {
        uv_close((uv_handle_t*)&g_server, NULL);
      }
      uv_close((uv_handle_t*)&g_tls_server, NULL);
      return -1;
    }

    tls_r = uv_listen((uv_stream_t*)&g_tls_server, 128, on_new_connection);
    if (tls_r != 0) {
      stump_er("uv_listen TLS: %s", uv_strerror(tls_r));
      if (server_config.bind_enabled) {
        uv_close((uv_handle_t*)&g_server, NULL);
      }
      uv_close((uv_handle_t*)&g_tls_server, NULL);
      return -1;
    }

    stump_i("listening on TLS %s:%d", server_config.tls_host, server_config.tls_port);
  }

  uv_signal_init(loop, &g_sigint);
  uv_signal_start(&g_sigint, on_signal, SIGINT);

  uv_signal_init(loop, &g_sigterm);
  uv_signal_start(&g_sigterm, on_signal, SIGTERM);

  return 0;
}
