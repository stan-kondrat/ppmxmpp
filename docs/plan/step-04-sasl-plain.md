# Step 4 — SASL PLAIN finished and verified

**Status: ✅ DONE**

## What

Complete the PLAIN handler: reject pre-TLS, decode base64, parse three null-separated fields, look up user, constant-time compare, emit `<success/>` or `<failure>`. Cap failed attempts per connection.

## Specs

- **RFC 6120 §6** — SASL profile for XMPP.
- **RFC 4616** — SASL PLAIN mechanism.

## Implementation

Full RFC 4616 SASL PLAIN implementation in `src/xmpp_sasl.c`: base64 decode, three-field NUL-delimited frame parse, authzid equality check, RFC 7622 localpart forbidden-character validation, constant-time `ct_memeq()` password comparison, disabled-account check. Queries SQLite. 13 unit tests in `tests/test_xmpp_sasl.c`.

PLAIN before STARTTLS is effectively rejected because the mechanism is only advertised post-TLS (state machine in Step 3).

`failed_auth_count` field added to `xmpp_session_t` (`include/xmpp.h`). On each `<failure/>`, the counter is incremented and the parser is reset so the client may reopen the stream and retry. On the third failure the server sends `<policy-violation/>` and closes. Verified by e2e test `test_e2e/sasl_auth_failure_cap.sh`.

## Done criteria

- [x] Real client authenticates as the seeded test user.
- [x] Wrong password yields `<failure><not-authorized/></failure>`.
- [x] PLAIN before STARTTLS is rejected (mechanism not advertised on plaintext stream).
- [x] Three failures close the stream with `<policy-violation/>`.
- [x] After `<success/>`, the parser resets and the client's next `<stream:stream>` is accepted.
