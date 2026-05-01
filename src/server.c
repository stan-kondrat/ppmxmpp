#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "config.h"
#include "log.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "server.h"
#include "tls.h"
#include "xmpp.h"

#include <sys/stat.h>

#define READ_BUF_SIZE 65536
/* TLS input buffer: must hold at least one full TLS record (max 16 KiB + header). */
#define TLS_IN_BUF_SIZE (READ_BUF_SIZE + 16384)

/* Per-connection state.  client must be first so (conn_t *) == (uv_handle_t *).
 */
typedef struct {
  uv_tcp_t client;
  char buf[READ_BUF_SIZE];
  uint64_t id;
  xmpp_session_t xmpp;
  /* TLS state — only valid when is_tls == 1 */
  int is_tls;
  int tls_hs_done;
  mbedtls_ssl_context ssl;
  unsigned char tls_in_buf[TLS_IN_BUF_SIZE];
  size_t tls_in_len;
  size_t tls_in_pos;
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
static tls_server_ctx_t g_tls_ctx;
static int g_tls_ctx_ready = 0;

/* ------------------------------------------------------------------ close */

static void on_conn_close(uv_handle_t* handle) {
  conn_t* conn = (conn_t*)handle;
  if (conn->is_tls) {
    mbedtls_ssl_free(&conn->ssl);
  }
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

/* Queue len bytes for async send via libuv. Data is copied. */
static int uv_conn_write(conn_t* conn, const char* data, size_t len) {
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

/* ------------------------------------------------------------------ TLS bio */

/* mbedtls bio send: queue encrypted bytes via libuv (always returns len). */
static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
  conn_t* conn = (conn_t*)ctx;
  write_req_t* wr = malloc(sizeof(*wr) + len);
  if (!wr) {
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  memcpy(wr->data, buf, len);
  wr->buf = uv_buf_init(wr->data, (unsigned int)len);
  if (uv_write(&wr->req, (uv_stream_t*)&conn->client, &wr->buf, 1, write_cb) < 0) {
    free(wr);
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return (int)len;
}

/* mbedtls bio recv: consume bytes from the TLS input buffer. */
static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
  conn_t* conn = (conn_t*)ctx;
  size_t avail = conn->tls_in_len - conn->tls_in_pos;
  if (avail == 0) {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  size_t n = avail < len ? avail : len;
  memcpy(buf, conn->tls_in_buf + conn->tls_in_pos, n);
  conn->tls_in_pos += n;
  return (int)n;
}

/* Initialise per-connection TLS on an already-accepted TCP connection.
 * Sets conn->is_tls = 1; handshake is driven lazily from read_cb. */
static int conn_init_tls(conn_t* conn) {
  mbedtls_ssl_init(&conn->ssl);
  int r = mbedtls_ssl_setup(&conn->ssl, &g_tls_ctx.conf);
  if (r != 0) {
    stump_er("conn %" PRIu64 ": ssl_setup failed: -0x%04x", conn->id, -r);
    mbedtls_ssl_free(&conn->ssl);
    return -1;
  }
  mbedtls_ssl_set_bio(&conn->ssl, conn, bio_send, bio_recv, NULL);
  conn->is_tls = 1;
  conn->tls_hs_done = 0;
  conn->tls_in_len = 0;
  conn->tls_in_pos = 0;
  return 0;
}

/* ------------------------------------------------------------------ write (XMPP callback) */

/* Send len bytes to conn. Uses mbedtls_ssl_write for TLS connections,
 * uv_write for plaintext. */
static int conn_write(void* ud, const char* data, size_t len) {
  conn_t* conn = (conn_t*)ud;

  if (conn->is_tls && conn->tls_hs_done) {
    const unsigned char* p = (const unsigned char*)data;
    size_t remaining = len;
    while (remaining > 0) {
      int r = mbedtls_ssl_write(&conn->ssl, p, remaining);
      if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        continue;
      }
      if (r <= 0) {
        stump_er("conn %" PRIu64 ": ssl_write failed: -0x%04x", conn->id, -r);
        return -1;
      }
      p += (size_t)r;
      remaining -= (size_t)r;
    }
    return 0;
  }

  return uv_conn_write(conn, data, len);
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

  /* ----- TLS path ----- */
  if (conn->is_tls) {
    /* Compact consumed bytes, then append new data. */
    if (conn->tls_in_pos > 0) {
      size_t rem = conn->tls_in_len - conn->tls_in_pos;
      if (rem > 0) {
        memmove(conn->tls_in_buf, conn->tls_in_buf + conn->tls_in_pos, rem);
      }
      conn->tls_in_len = rem;
      conn->tls_in_pos = 0;
    }
    if (conn->tls_in_len + (size_t)nread > TLS_IN_BUF_SIZE) {
      stump_er("conn %" PRIu64 ": TLS input buffer overflow", conn->id);
      close_conn(conn);
      return;
    }
    memcpy(conn->tls_in_buf + conn->tls_in_len, buf->base, (size_t)nread);
    conn->tls_in_len += (size_t)nread;

    /* Drive the TLS handshake until it completes or needs more data. */
    if (!conn->tls_hs_done) {
      int r = mbedtls_ssl_handshake(&conn->ssl);
      if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return; /* wait for next read_cb */
      }
      if (r != 0) {
        stump_er("conn %" PRIu64 ": TLS handshake failed: -0x%04x", conn->id, -r);
        close_conn(conn);
        return;
      }
      stump_i("conn %" PRIu64 ": TLS handshake complete", conn->id);
      conn->tls_hs_done = 1;
    }

    /* Decrypt available records and feed plaintext to XMPP. */
    char plain[READ_BUF_SIZE];
    int r = mbedtls_ssl_read(&conn->ssl, (unsigned char*)plain, sizeof(plain));
    if (r == MBEDTLS_ERR_SSL_WANT_READ) {
      return;
    }
    if (r <= 0) {
      if (r != 0) {
        stump_d("conn %" PRIu64 ": ssl_read: -0x%04x", conn->id, -r);
      }
      close_conn(conn);
      return;
    }

    int rc = xmpp_feed(&conn->xmpp, plain, (size_t)r, conn_write, conn);
    if (rc != 0) {
      stump_d("conn %" PRIu64 ": XMPP requested close", conn->id);
      close_conn(conn);
    }
    return;
  }

  /* ----- Plaintext path ----- */
  int rc = xmpp_feed(&conn->xmpp, buf->base, (size_t)nread, conn_write, conn);
  if (rc != 0) {
    stump_d("conn %" PRIu64 ": XMPP requested close", conn->id);
    close_conn(conn);
    return;
  }

  /* STARTTLS in-place upgrade: after <proceed/> is sent the XMPP state
   * transitions to TLS_HANDSHAKING.  Init TLS now; the ClientHello will
   * arrive in the next read_cb and drive the handshake. */
  if (conn->xmpp.state == XMPP_STATE_TLS_HANDSHAKING && !conn->is_tls) {
    if (conn_init_tls(conn) != 0) {
      close_conn(conn);
    }
  }
}

/* ------------------------------------------------------------------ accept */

static void accept_conn(uv_stream_t* server, int status, int is_tls) {
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

  if (is_tls) {
    /* Direct-TLS port: perform TLS handshake before any XMPP.
     * Start XMPP state at TLS_HANDSHAKING so the first stream:stream open
     * transitions to STREAM_OPENED_TLS (not STREAM_OPENED_PLAINTEXT). */
    conn->xmpp.state = XMPP_STATE_TLS_HANDSHAKING;
    if (conn_init_tls(conn) != 0) {
      uv_close((uv_handle_t*)&conn->client, on_conn_close);
      return;
    }
    stump_i("conn %" PRIu64 ": accepted (TLS)", conn->id);
  } else {
    stump_i("conn %" PRIu64 ": accepted", conn->id);
  }

  if (uv_read_start((uv_stream_t*)&conn->client, alloc_cb, read_cb) != 0) {
    stump_er("conn %" PRIu64 ": uv_read_start failed", conn->id);
    close_conn(conn);
  }
}

static void on_new_connection(uv_stream_t* server, int status) { accept_conn(server, status, 0); }

static void on_new_tls_connection(uv_stream_t* server, int status) {
  accept_conn(server, status, 1);
}

/* ------------------------------------------------------------------ shutdown */

static void close_walk_cb(uv_handle_t* handle, void* arg) {
  (void)arg;
  if (!uv_is_closing(handle)) {
    /* handle->data is non-NULL only for conn_t handles (set in accept_conn).
     * Server and signal handles leave data NULL, so they get a no-op callback. */
    uv_close(handle, handle->data ? on_conn_close : NULL);
  }
}

static void on_signal(uv_signal_t* handle, int signum) {
  stump_i("signal %d received, shutting down", signum);
  uv_walk(handle->loop, close_walk_cb, NULL);
  if (g_tls_ctx_ready) {
    tls_server_ctx_free(&g_tls_ctx);
    g_tls_ctx_ready = 0;
  }
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
      stump_i("TLS enabled but cert/key missing, generating self-signed certificate");
      if (generate_self_signed_cert(server_config.tls_cert_file, server_config.tls_key_file) != 0) {
        stump_er("failed to generate self-signed certificate");
        return -1;
      }
    }

    if (tls_server_ctx_init(&g_tls_ctx, server_config.tls_cert_file, server_config.tls_key_file) !=
        0) {
      stump_er("failed to initialize TLS server context");
      return -1;
    }
    g_tls_ctx_ready = 1;

    struct sockaddr_in tls_addr;
    int tls_r = uv_ip4_addr(server_config.tls_host, server_config.tls_port, &tls_addr);
    if (tls_r != 0) {
      stump_er("invalid TLS address %s:%d: %s", server_config.tls_host, server_config.tls_port,
               uv_strerror(tls_r));
      if (server_config.bind_enabled) {
        uv_close((uv_handle_t*)&g_server, NULL);
      }
      tls_server_ctx_free(&g_tls_ctx);
      g_tls_ctx_ready = 0;
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
      tls_server_ctx_free(&g_tls_ctx);
      g_tls_ctx_ready = 0;
      return -1;
    }

    tls_r = uv_listen((uv_stream_t*)&g_tls_server, 128, on_new_tls_connection);
    if (tls_r != 0) {
      stump_er("uv_listen TLS: %s", uv_strerror(tls_r));
      if (server_config.bind_enabled) {
        uv_close((uv_handle_t*)&g_server, NULL);
      }
      uv_close((uv_handle_t*)&g_tls_server, NULL);
      tls_server_ctx_free(&g_tls_ctx);
      g_tls_ctx_ready = 0;
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
