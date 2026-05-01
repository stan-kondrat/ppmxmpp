# Step 5 — Resource binding

**Status: 🔶 PARTIAL — resource not extracted or stored**

## What

Handle `<iq type="set"><bind>...</bind></iq>`. Accept the client's proposed resource if present and valid; otherwise generate one (8 hex characters via mbedTLS CTR-DRBG). Validate length ≤ 1023 bytes and ASCII-only (libidn2 stub). Return the full bound JID.

## Specs

- **RFC 6120 §7** — resource binding.
- **RFC 7622** — JID profile (stubbed; full PRECIS deferred).

## Current state

The `RESOURCE_BOUND` state transition is implemented. The server handles `<iq type='set'><bind>` and responds with `<iq type='result'>`, moving to `RESOURCE_BOUND` state.

**Gap:** The `<jid>` in the result returns `user@domain` without a resource part. The client's `<resource>` child is not extracted or stored. No auto-generated resource when client omits it. The full bound JID is not stored on `conn_t` for use in routing (Steps 8–10).

## What remains

- Parse `<resource>` child element from the bind IQ stanza.
- Validate: length ≤ 1023 bytes, no forbidden characters per RFC 7622.
- Generate a random 8-hex-char resource via `mbedtls_ctr_drbg_random()` when client omits it.
- Return `user@domain/resource` in the result `<jid>` element.
- Store the full bound JID on `conn_t` / `xmpp_session_t` for later routing.

## Done criteria

- [ ] Real client completes bind and shows the bound JID including resource.
- [ ] Bind with oversized or non-ASCII resource yields a bind error per §7.7.
- [ ] Connection state holds the full JID for later routing.
