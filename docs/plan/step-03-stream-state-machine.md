# Step 3 — Stream negotiation state machine

**Status: ✅ DONE**

## What

Implement the C2S negotiation states with explicit transitions:

```
CONNECTED → STREAM_OPENED_PLAINTEXT → TLS_HANDSHAKING → STREAM_OPENED_TLS
          → SASL_AUTHENTICATING → STREAM_OPENED_AUTHENTICATED → RESOURCE_BOUND → CLOSING → CLOSED
```

Advertise the right `<stream:features>` at each stage:
- Plaintext stream: only `<starttls/>`, marked required.
- Post-TLS stream: SASL mechanisms, marked required.
- Post-SASL stream: `<bind/>`, marked required.

Out-of-order stanzas produce a stream error and close.

## Specs

- **RFC 6120 §4** — streams.
- **RFC 6120 §5** — STARTTLS negotiation.
- **RFC 6120 §6** — SASL negotiation.
- **RFC 6120 §7** — resource binding (advertisement only in this step).
- **RFC 6120 §4.9** — stream errors.

## Current state

Full 9-state machine implemented in `src/xmpp.c`. `_validate_transition()` enforces legal transitions. `_send_stream_features()` sends the correct feature set at each state. Out-of-order stanzas produce stream errors. 20 unit tests in `tests/test_xmpp_state.c` covering all transitions, feature advertisement, error cases, and multi-reset flows.

## Done criteria

- [x] Real client opens stream, sees `<starttls/>` required, completes STARTTLS.
- [x] After TLS, client sees SASL mechanisms.
- [x] After SASL, client sees `<bind/>`.
- [x] Sending bind before SASL produces a stream error and clean close.
- [x] Every transition logged at DEBUG with from/to state and connection ID.
