# Step 11 — Offline messages

**Status: ✅ DONE**

## What

Store messages whose target has no available resource and deliver them on the recipient's next login, before any further stanzas.

- SQLite table `offline_messages` (id, recipient_bare_jid, sender_jid, stanza_xml, received_at).
- On message routing failure due to no available resource: insert into `offline_messages`.
- On successful resource bind for a user: drain offline messages in `received_at` order, sending each as a normal message stanza with a `<delay>` stamp, then delete.
- Reasonable cap (100 messages or 1 MB per user) with overflow returning `<service-unavailable/>` to the sender.

## Specs

- **XEP-0160** — Best Practices for Handling Offline Messages.
- **XEP-0203** — Delayed Delivery: stamp delivered offline messages with `<delay xmlns="urn:xmpp:delay" stamp="..."/>` so the recipient knows the original time.

## Current state

No offline message storage. No `offline_messages` table. The SQLite schema only has `users` (version 1 migration).

## What to build

- Schema migration (version 3, or combined with Step 7 migration): create `offline_messages` table.
- `src/storage/offline.c` — insert, count, list, delete functions.
- Hook into message routing (Step 10): on no-resource failure, call `offline_store()`.
- Hook into resource bind success: call `offline_drain()`, send each stored message with `<delay>` stamp, delete after sending.
- Enforce cap: if count or size exceeds limit, return `<service-unavailable/>` error stanza to sender.

## Done criteria

- [x] A sends to B while B is offline; B logs in and receives the message stamped with the original time.
- [x] Capped storage rejects with `<service-unavailable/>` after the cap is reached.
