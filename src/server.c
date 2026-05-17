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
#include "storage/db.h"
#include "tls.h"
#include "xep-0030-service-discovery.h"
#include "xep-0054-vcard.h"
#include "xep-0184-receipts.h"
#include "xep-0186-blocking.h"
#include "xep-0199-ping.h"
#include "xep-0280-carbons.h"
#include "xmpp.h"
#include "xmpp_iq_dispatch.h"
#include "xmpp_presence.h"

#include <sys/stat.h>

#define READ_BUF_SIZE 16384
/* TLS record max payload is 16 KiB; add 5-byte header + 256-byte MAC overhead. */
#define TLS_IN_BUF_SIZE (16384 + 512)

/* Per-connection state.  client must be first so (conn_t *) == (uv_handle_t *).
 */
typedef struct {
  uv_tcp_t client;
  char buf[READ_BUF_SIZE];
  uint64_t id;
  xmpp_session_t xmpp;
  /* TLS state — used for STARTTLS upgrade */
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
static uv_signal_t g_sigint;
static uv_signal_t g_sigterm;
static _Atomic uint64_t g_next_id = 1;
static tls_server_ctx_t g_tls_ctx;
static int g_tls_ctx_ready = 0;

static int conn_write(void* ud, const char* data, size_t len);

/* ------------------------------------------------------------------ close */

static void on_conn_close(uv_handle_t* handle) {
  conn_t* conn = (conn_t*)handle;

  /* RFC 6121 §4.4.2: broadcast unavailable on ungraceful disconnect if the
   * session had sent initial presence (registered in the presence table). */
  if (conn->xmpp.bound_jid[0] != '\0') {
    char bare_jid[JID_BUF_SIZE];
    const char* slash = strchr(conn->xmpp.bound_jid, '/');
    size_t bare_len = slash ? (size_t)(slash - conn->xmpp.bound_jid)
                            : strlen(conn->xmpp.bound_jid);
    if (bare_len >= sizeof(bare_jid)) bare_len = sizeof(bare_jid) - 1;
    memcpy(bare_jid, conn->xmpp.bound_jid, bare_len);
    bare_jid[bare_len] = '\0';
    xmpp_presence_on_disconnect(conn->xmpp.bound_jid, bare_jid);
  }

  if (conn->is_tls) {
    mbedtls_ssl_free(&conn->ssl);
  }
  xmpp_session_cleanup(&conn->xmpp);
  stump_d("conn %" PRIu64 ": connection freed", conn->id);
  free(conn);
}

/* Drain any remaining buffered TLS data before closing.
 * This ensures messages that arrive just before the connection closes
 * are still processed (e.g., a message sent before a /quit). */
static void drain_pending_tls(conn_t* conn) {
  if (!conn->is_tls || !conn->tls_hs_done) return;

  while (1) {
    char plain[READ_BUF_SIZE];
    int r = mbedtls_ssl_read(&conn->ssl, (unsigned char*)plain, sizeof(plain));
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
      break;
    }
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      stump_d("conn %" PRIu64 ": drain_pending_tls: peer close-notify", conn->id);
      break;
    }
    if (r <= 0) {
      break;
    }
    stump_d("conn %" PRIu64 ": drain_pending_tls: decrypted %d bytes", conn->id, r);
    /* Debug: log the raw decrypted XML (first 512 chars) */
    {
      char _dbgbuf2[513];
      size_t _dbglen2 = (size_t)r < 512 ? (size_t)r : 512;
      memcpy(_dbgbuf2, plain, _dbglen2);
      _dbgbuf2[_dbglen2] = '\0';
      stump_d("conn %" PRIu64 ": drain_raw: '%s'", conn->id, _dbgbuf2);
    }
    /* Feed remaining data to parser. Ignore return — we are draining. */
    (void)xmpp_feed(&conn->xmpp, plain, (size_t)r, conn_write, conn);
  }
}

static void close_conn(conn_t* conn) {
  if (!uv_is_closing((uv_handle_t*)&conn->client)) {
    /* Drain any pending TLS data before closing so that stanzas received
     * just before the close (e.g., a message sent right before /quit) are
     * still processed. */
    drain_pending_tls(conn);
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
    /* UV_EOF: drain any remaining bytes that arrived just before the FIN,
     * then close.  Without draining, bytes that were in the kernel buffer
     * when the FIN was received can be lost (e.g. a message sent right
     * before /quit). */
    drain_pending_tls(conn);
    close_conn(conn);
    return;
  }

  if (nread == 0) {
    return;
  }

  /* ----- STARTTLS upgrade path ----- */
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
      conn->xmpp.state = XMPP_STATE_TLS_NEGOTIATED;
      conn->xmpp.needs_parser_reset = 1;
    }

    /* Decrypt and feed all TLS records already buffered in the SSL context.
     * mbedtls_ssl_check_pending() returns 1 when the input buffer holds a
     * complete record that has not been returned by ssl_read yet, so we drain
     * those without blocking on the network.  The first read always runs; after
     * that we only loop while there is more buffered data. */
    do {
      char plain[READ_BUF_SIZE];
      int r = mbedtls_ssl_read(&conn->ssl, (unsigned char*)plain, sizeof(plain));
      if (r == MBEDTLS_ERR_SSL_WANT_READ) {
        break;
      }
      if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        stump_d("conn %" PRIu64 ": TLS close-notify received", conn->id);
        /* Drain remaining TLS data before closing — there may still be bytes
         * in the mbedtls input buffer that were received just before the
         * close-notify (e.g., a message sent immediately before /quit). */
        drain_pending_tls(conn);
        close_conn(conn);
        return;
      }
      if (r <= 0) {
        if (r != 0) {
          stump_d("conn %" PRIu64 ": ssl_read: -0x%04x", conn->id, -r);
        }
        drain_pending_tls(conn);
        close_conn(conn);
        return;
      }
      stump_d("conn %" PRIu64 ": ssl_read: decrypted %d bytes", conn->id, r);
      /* Debug: log the raw decrypted XML (first 512 chars) */
      {
        char _dbgbuf[513];
        size_t _dbglen = (size_t)r < 512 ? (size_t)r : 512;
        memcpy(_dbgbuf, plain, _dbglen);
        _dbgbuf[_dbglen] = '\0';
        stump_d("conn %" PRIu64 ": raw_decrypted: '%s'", conn->id, _dbgbuf);
      }
      int rc = xmpp_feed(&conn->xmpp, plain, (size_t)r, conn_write, conn);
      if (rc != 0) {
        stump_d("conn %" PRIu64 ": XMPP requested close", conn->id);
        close_conn(conn);
        return;
      }
    } while (mbedtls_ssl_check_pending(&conn->ssl) ||
             (conn->tls_in_len > conn->tls_in_pos));
    stump_d("conn %" PRIu64 ": TLS read loop done, tls_in_buf: pos=%zu len=%zu remaining=%zu",
            conn->id, conn->tls_in_pos, conn->tls_in_len,
            conn->tls_in_len - conn->tls_in_pos);
    return;
  }

  /* ----- Plaintext path ----- */
  int rc = xmpp_feed(&conn->xmpp, buf->base, (size_t)nread, conn_write, conn);
  if (rc != 0) {
    stump_d("conn %" PRIu64 ": XMPP requested close", conn->id);
    close_conn(conn);
    return;
  }

  /* STARTTLS upgrade: after <proceed/> is sent, needs_starttls_proceed is set.
   * Init TLS now; the ClientHello will arrive in the next read_cb. */
  if (conn->xmpp.needs_starttls_proceed && !conn->is_tls) {
    conn->xmpp.needs_starttls_proceed = 0;
    if (!g_tls_ctx_ready) {
      stump_er("conn %" PRIu64 ": STARTTLS requested but no TLS context", conn->id);
      close_conn(conn);
      return;
    }
    if (conn_init_tls(conn) != 0) {
      close_conn(conn);
    }
  }
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
  /* Init TLS context when cert/key are configured — needed for STARTTLS. */
  if (server_config.tls_cert_file[0] != '\0' && server_config.tls_key_file[0] != '\0') {
    if (!file_exists(server_config.tls_cert_file) || !file_exists(server_config.tls_key_file)) {
      stump_i("TLS cert/key missing, generating self-signed certificate");
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
    stump_i("TLS context ready (STARTTLS available)");
  }

  struct sockaddr_in addr;
  int r = uv_ip4_addr(server_config.bind_host, server_config.bind_port, &addr);
  if (r != 0) {
    stump_er("invalid listen address %s:%d: %s", server_config.bind_host, server_config.bind_port,
             uv_strerror(r));
    goto fail_tls;
  }

  uv_tcp_init(loop, &g_server);
  /* g_server.data stays NULL — see close_walk_cb */

  r = uv_tcp_bind(&g_server, (const struct sockaddr*)&addr, 0);
  if (r != 0) {
    stump_er("uv_tcp_bind %s:%d: %s", server_config.bind_host, server_config.bind_port,
             uv_strerror(r));
    uv_close((uv_handle_t*)&g_server, NULL);
    goto fail_tls;
  }

  r = uv_listen((uv_stream_t*)&g_server, 128, on_new_connection);
  if (r != 0) {
    stump_er("uv_listen: %s", uv_strerror(r));
    uv_close((uv_handle_t*)&g_server, NULL);
    goto fail_tls;
  }

  stump_i("listening on %s:%d%s", server_config.bind_host, server_config.bind_port,
          g_tls_ctx_ready ? " (TLS ready)" : "");

  uv_signal_init(loop, &g_sigint);
  uv_signal_start(&g_sigint, on_signal, SIGINT);

  uv_signal_init(loop, &g_sigterm);
  uv_signal_start(&g_sigterm, on_signal, SIGTERM);

  return 0;

fail_tls:
  if (g_tls_ctx_ready) {
    tls_server_ctx_free(&g_tls_ctx);
    g_tls_ctx_ready = 0;
  }
  return -1;
}

void server_tls_cleanup(void) {
  if (g_tls_ctx_ready) {
    tls_server_ctx_free(&g_tls_ctx);
    g_tls_ctx_ready = 0;
  }
}

int server_init(void) {
  log_init();  /* idempotent if already called */
  if (iq_dispatch_init() != 0) {
    stump_er("server_init: iq_dispatch_init failed");
    log_free();
    return -1;
  }
  if (xmpp_iq_register_handlers() != 0) {
    stump_er("server_init: xmpp_iq_register_handlers failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  if (xep0030_init() != 0) {
    stump_er("server_init: xep0030_init failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  if (xep0199_init() != 0) {
    stump_er("server_init: xep0199_init failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  if (xep0280_init() != 0) {
    stump_er("server_init: xep0280_init failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  if (xep0184_init() != 0) {
    stump_er("server_init: xep0184_init failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  if (xep0054_init() != 0) {
    stump_er("server_init: xep0054_init failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  if (xep0186_init() != 0) {
    stump_er("server_init: xep0186_init failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  sqlite3* db = NULL;
  if (storage_db_open(&db) != 0) {
    stump_er("server_init: storage_db_open failed");
    iq_dispatch_shutdown();
    log_free();
    return -1;
  }
  return 0;
}

void server_shutdown(void) {
  storage_db_close();
  iq_dispatch_shutdown();
  log_free();
}

