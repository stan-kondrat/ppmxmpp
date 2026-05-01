# Step 12 — Message carbons (recommended for multi-device)

**Status: ❌ NOT DONE**

## What

Almost every modern client connects from multiple devices (phone + desktop). Without carbons, only one device sees a given conversation. Implement carbons so chats appear on every active resource.

- Per-resource opt-in via `<iq type="set"><enable xmlns="urn:xmpp:carbons:2"/></iq>`.
- When delivering a chat message to one resource of a user, also deliver a `<sent>`/`<received>` wrapped copy to every other carbon-enabled resource of that user.

## Specs

- **XEP-0280** — Message Carbons.
- **XEP-0297** — Stanza Forwarding (carbons depend on this wrapper format).

## Current state

Not implemented. No carbon opt-in handling, no carbon-enabled flag per session, no forwarded stanza wrapping.

## What to build

- `carbons_enabled` boolean flag on `conn_t` / session struct.
- IQ handler for `urn:xmpp:carbons:2` enable/disable.
- In the message routing path (Step 10): after delivering to the primary resource, iterate other resources of the same user that have carbons enabled and send a `<message>` wrapping `<forwarded xmlns='urn:xmpp:forward:0'>` with a `<sent>` or `<received>` element.

## Done criteria

- [ ] User logged in on two clients with carbons enabled sees the same conversation thread on both.
- [ ] A message sent from one device appears on the other as `<sent>`.
