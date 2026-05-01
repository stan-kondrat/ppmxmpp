# Step 4 — SASL PLAIN finished and verified

**Status: 🔶 PARTIAL — missing failure cap**

## What

Complete the PLAIN handler: reject pre-TLS, decode base64, parse three null-separated fields, look up user, constant-time compare, emit `<success/>` or `<failure>`. Cap failed attempts per connection.

## Specs

- **RFC 6120 §6** — SASL profile for XMPP.
- **RFC 4616** — SASL PLAIN mechanism.

## Current state

Full RFC 4616 SASL PLAIN implementation in `src/xmpp_sasl.c`: base64 decode, three-field NUL-delimited frame parse, authzid equality check, RFC 7622 localpart forbidden-character validation, constant-time `ct_memeq()` password comparison, disabled-account check. Queries SQLite. 13 unit tests in `tests/test_xmpp_sasl.c`.

PLAIN before STARTTLS is effectively rejected because the mechanism is only advertised post-TLS (state machine in Step 3).

**Gap:** No per-connection failed-authentication counter — three failures do not close the stream.

## What remains

- Add a `failed_auth_count` field to `conn_t` (or `xmpp_session_t`).
- Increment on each `<failure/>` response.
- Close the stream with `<policy-violation/>` after 3 failures.

## Done criteria

- [x] Real client authenticates as the seeded test user.
- [x] Wrong password yields `<failure><not-authorized/></failure>`.
- [x] PLAIN before STARTTLS is rejected (mechanism not advertised on plaintext stream).
- [ ] Three failures close the stream.
- [x] After `<success/>`, the parser resets and the client's next `<stream:stream>` is accepted.
