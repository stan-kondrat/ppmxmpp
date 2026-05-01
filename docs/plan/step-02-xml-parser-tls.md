# Step 2 — Per-connection XML parser wired to the TLS read path

**Status: 🔶 PARTIAL**

## What

For each connection, create a libstrophe parser context. Feed plaintext bytes from `mbedtls_ssl_read` into the parser. Hook stream-open and stanza callbacks. Implement `parser_reset()` to be called after STARTTLS and after SASL success.

## Specs

- **RFC 6120 §4** — XML streams: opening tag, namespaces, restart semantics.
- **RFC 6120 §11.2** — XML restrictions (no comments, no PIs, no DOCTYPE).

## Current state

The XML parser (libstrophe/expat) is per-connection and wired to the raw TCP read path (`xmpp_feed()` called from `read_cb` in `server.c`). `parser_reset()` is correctly called after STARTTLS and after SASL success (`xmpp.c`). Malformed XML and stream:error handling is implemented.

**Critical gap:** No actual TLS handshake or encryption per connection. `tls.c` only generates the self-signed certificate — there is no `mbedtls_ssl_context` attached to `conn_t`. The TLS port in `server.c` accepts raw TCP connections and passes unencrypted bytes directly to `xmpp_feed()`. A real client that attempts a TLS handshake will fail immediately.

## What remains

- Allocate `mbedtls_ssl_context` + `mbedtls_ssl_config` per `conn_t` on the TLS port.
- Perform `mbedtls_ssl_handshake()` before feeding bytes to `xmpp_feed()`.
- Replace direct libuv buffer reads with `mbedtls_ssl_read()`.
- Wire `mbedtls_ssl_write()` for outbound data on TLS connections.
- Handle the STARTTLS in-place upgrade: after sending `<proceed/>`, perform the TLS handshake on the existing TCP connection, then reset the parser.

## Done criteria

- [ ] A real client's `<stream:stream>` opens the parser without error over a real TLS connection.
- [x] After STARTTLS, the client's second `<stream:stream>` is parsed correctly (parser was reset).
- [x] Malformed XML emits `<not-well-formed/>` stream error and closes.
