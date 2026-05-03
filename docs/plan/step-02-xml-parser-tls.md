# Step 2 — Per-connection XML parser wired to the TLS read path

**Status: ✅ DONE**

## What

For each connection, create a libstrophe parser context. Feed plaintext bytes from `mbedtls_ssl_read` into the parser. Hook stream-open and stanza callbacks. Implement `parser_reset()` to be called after STARTTLS and after SASL success.

## Specs

- **RFC 6120 §4** — XML streams: opening tag, namespaces, restart semantics.
- **RFC 6120 §11.2** — XML restrictions (no comments, no PIs, no DOCTYPE).

## Current state

The XML parser (libstrophe/expat) is per-connection and wired to the read path (`xmpp_feed()` called from `read_cb` in `server.c`). `parser_reset()` is correctly called after STARTTLS and after SASL success (`xmpp.c`). Malformed XML and stream:error handling is implemented.

Per-connection TLS is fully implemented via mbedTLS: `conn_t` holds an `mbedtls_ssl_context`, the handshake is driven in `read_cb`, and all outbound data goes through `mbedtls_ssl_write()`. The in-place STARTTLS upgrade (send `<proceed/>`, perform TLS handshake on the existing TCP connection, reset parser) works end-to-end with real clients.

## Done criteria

- [x] A real client's `<stream:stream>` opens the parser without error over a real TLS connection.
- [x] After STARTTLS, the client's second `<stream:stream>` is parsed correctly (parser was reset).
- [x] Malformed XML emits `<not-well-formed/>` stream error and closes.

## E2E tests

`test_e2e/profanity_connect.sh` — drives profanity (0.17.0) against a live server instance:
1. Generates a self-signed cert and seeds a test user in SQLite.
2. Starts `build/debug/ppmxmpp` on a free ephemeral port.
3. Pre-trusts the cert in profanity's `tlscerts` keyfile (SHA-256 fingerprint).
4. Runs profanity via `script(1)` (PTY required for curses init) with `tls_policy=allow`.
5. Polls the profanity debug log for `logged in` / `session established`.

`test_e2e/tls_connection.sh` — verifies the server loads an externally-generated cert and the port is TCP-reachable via `openssl s_client`.

`test_e2e/tls_auto_generation.sh` — verifies the server auto-generates a self-signed cert (CN=localhost, SAN DNS:localhost) when none is present.
