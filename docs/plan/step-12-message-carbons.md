# Step 12 — Message carbons (recommended for multi-device)

**Status: ✅ DONE**

## What

Almost every modern client connects from multiple devices (phone + desktop). Without carbons, only one device sees a given conversation. Implement carbons so chats appear on every active resource.

- Per-resource opt-in via `<iq type="set"><enable xmlns="urn:xmpp:carbons:2"/></iq>`.
- When delivering a chat message to one resource of a user, also deliver a `<sent>`/`<received>` wrapped copy to every other carbon-enabled resource of that user.

## Specs

- **XEP-0280** — Message Carbons.
- **XEP-0297** — Stanza Forwarding (carbons depend on this wrapper format).

## Implementation

- `carbons_enabled` flag on both `xmpp_session_t` (connection ctx) and `session_entry_t` (session table).
  - Flag is set on `xmpp_session_t` in the IQ handler so it survives until the session is registered in the table on initial presence. `xmpp_session_table_register` copies it into the table entry.
  - `xmpp_session_table_update_carbons` updates the table entry for already-registered sessions.
- IQ handler in `xmpp_iq.c` for `urn:xmpp:carbons:2` enable/disable; replies with `<iq type='result'/>`.
- `xep-0280-carbons.c`: `xmpp_session_table_for_each_carbon_resource` iterates the session table for carbons-enabled resources of a bare JID, excluding a specified full JID.
- `xmpp_message.c`: after routing, calls `send_carbon_sent` (for all other carbons-enabled resources of the sender) and `send_carbon_received` (for all other carbons-enabled resources of the recipient). Both wrap the original stanza in `<sent|received xmlns='urn:xmpp:carbons:2'><forwarded xmlns='urn:xmpp:forward:0'>`.
- `<private xmlns='urn:xmpp:carbons:2'/>` in a stanza suppresses carbon copies (XEP-0280 §9).
- `urn:xmpp:carbons:2` and `urn:xmpp:forward:0` advertised in the server's disco#info feature list.

## Done criteria

- [x] User logged in on two clients with carbons enabled sees the same conversation thread on both.
- [x] A message sent from one device appears on the other as `<sent>`.
- [x] A message received by one device appears on all other carbons-enabled devices as `<received>`.
- [x] E2E test: `test_e2e/message_carbons.sh` — three-resource scenario (alice-desktop, alice-mobile, alice-server) validating `<sent>` and `<received>` carbons across phases.
- [x] Unit tests: `tests/xep-0280-carbons.c`.
