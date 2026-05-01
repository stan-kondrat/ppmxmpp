# Step 18 — Optional: OMEMO End-to-End Encryption

**Status: ❌ NOT DONE**

## What

OMEMO is the standard end-to-end encryption layer for XMPP. It is implemented entirely on the client side using the Signal Protocol (Double Ratchet + X3DH). The server's job is to store and distribute public key material (device bundles) via PEP and to route encrypted message payloads without decrypting them.

Server responsibilities:
- **PEP (Personal Eventing Protocol)** — clients publish their device list and key bundles as PEP nodes; other clients fetch them before starting a session.
- **PEP node storage** — persist and deliver PEP item publish/retract/subscribe stanzas.
- **Carbons + MAM compatibility** — encrypted `<message>` stanzas must be forwarded and archived like any other message (Steps 12 and 16 already handle this transparently).

The server does **not** implement the Signal Protocol itself — that is entirely client-side.

## Specs

- **XEP-0384** — OMEMO Encryption (current version; references Signal Protocol for the crypto).
- **XEP-0163** — Personal Eventing Protocol (PEP) — required for publishing device lists and key bundles.
- **XEP-0060** — Publish-Subscribe (pubsub) — PEP is a profile of pubsub; a minimal subset is sufficient.
- **XEP-0280** — Message Carbons (Step 12) — needed so encrypted messages reach all devices.
- **XEP-0313** — MAM (Step 16) — OMEMO messages are stored encrypted; no server-side plaintext.

## Current state

No PEP / pubsub infrastructure. No XEP-0384 support. Steps 12 and 16 (which OMEMO depends on) are also not yet done.

## What to build

- **Minimal pubsub / PEP node store**: SQLite table `pep_items` (owner_bare_jid, node, item_id, xml, updated_at). Support `publish`, `retract`, and `items` (fetch all items for a node).
- **PEP IQ handler**: wire into the IQ router (Step 6) for namespace `http://jabber.org/protocol/pubsub`.
- **PEP notification fanout**: when a user publishes to a PEP node, push a `<message>` notification to subscribers (for device-list nodes, subscribers are the user's own resources plus roster contacts with `both` subscription).
- **Device list node** (`urn:xmpp:omemo:2:devices`): clients publish here to advertise OMEMO device IDs.
- **Bundle node** (`urn:xmpp:omemo:2:bundles:<deviceId>`): clients publish their key bundle here; other clients fetch it before starting a session.
- No server-side crypto — just store and forward.

## Dependencies

- Step 7 (roster) — needed for subscription-based PEP fanout.
- Step 8 (presence routing) — needed to know which resources are active for notification delivery.
- Step 12 (carbons) — needed so encrypted messages reach all devices of a user.
- Step 16 (MAM) — needed for history sync of encrypted messages.

## Done criteria

- [ ] Client publishes its OMEMO device list; another client fetches it via PEP.
- [ ] Client publishes a key bundle; another client fetches it.
- [ ] Two OMEMO-capable clients exchange encrypted messages end-to-end (server sees only ciphertext).
