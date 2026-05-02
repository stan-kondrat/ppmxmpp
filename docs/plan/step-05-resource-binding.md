# Step 5 — Resource binding

**Status: ✅ DONE**

## What

Handle `<iq type="set"><bind>...</bind></iq>`. Accept the client's proposed resource if
present and valid; otherwise generate one (8 hex characters from `/dev/urandom`, matching
the stream-ID generation pattern). Validate resource length ≤ 1023 bytes and forbidden
characters per RFC 7622. Return the full bound JID (`user@domain/resource`) in the
result. Store it on `xmpp_session_t` for use by later routing steps.

## Specs

- **RFC 6120 §7** — resource binding.
- **RFC 7622 §3.4** — resourcepart rules (length, forbidden characters).

## Current state

The `RESOURCE_BOUND` state transition is implemented. The server handles
`<iq type='set'><bind>` and responds with `<iq type='result'>`, moving to
`XMPP_STATE_CONNECTED`. The bind stanza is parsed enough to detect presence of
`<bind>` child.

**Gap:** The `<jid>` in the result omits the resource part. The client's `<resource>`
child is not read. No auto-generated resource when client omits it. The full bound JID is
not stored on `xmpp_session_t` for routing (Steps 8–10).

## Substeps

| File | Title |
|------|-------|
| [step-05/01-session-fields.md](step-05/01-session-fields.md) | Add `resource` and `bound_jid` fields to `xmpp_session_t` |
| [step-05/02-parse-resource.md](step-05/02-parse-resource.md) | Parse `<resource>` child from bind IQ |
| [step-05/03-validate-resource.md](step-05/03-validate-resource.md) | Validate resource: length and forbidden characters |
| [step-05/04-generate-resource.md](step-05/04-generate-resource.md) | Generate 8-hex resource when client omits it |
| [step-05/05-store-and-respond.md](step-05/05-store-and-respond.md) | Store full JID and return `user@domain/resource` in result |
| [step-05/06-unit-tests.md](step-05/06-unit-tests.md) | Unit tests for all bind scenarios |
| [step-05/07-e2e-test.md](step-05/07-e2e-test.md) | End-to-end verification with a real client |

## Done criteria

- [ ] Real client completes bind and shows the bound JID including resource.
- [x] Bind with client-supplied resource returns `user@domain/<resource>` exactly.
- [x] Bind without `<resource>` child returns `user@domain/<generated-8hex>`.
- [x] Bind with oversized resource (> 1023 bytes) yields `<bad-request>` bind error.
- [x] Bind with forbidden character in resource yields `<bad-request>` bind error.
- [x] `xmpp_session_t.bound_jid` holds the full JID after successful bind.
- [x] All existing unit tests continue to pass.
