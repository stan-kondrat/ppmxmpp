# Step 16 — Optional: Message archive (MAM) for history sync

**Status: ❌ NOT DONE**

## What

Persist messages and let clients query history. Without MAM, a user logging in on a new device sees nothing of past conversations.

- SQLite table `archive` (id INTEGER PRIMARY KEY, owner_bare_jid TEXT, with_jid TEXT, direction TEXT, stanza_xml TEXT, ts INTEGER).
- Implement query handler for `urn:xmpp:mam:2` with at least date-range and `with` filters and result-set management.

## Specs

- **XEP-0313** — Message Archive Management.
- **XEP-0059** — Result Set Management (used by MAM queries for pagination).

## Current state

No archive table. No MAM handler. Messages are not persisted beyond offline delivery (Step 11).

## What to build

- Schema migration: create `archive` table.
- Hook into message routing (Step 10): write each delivered message to the archive for both sender and recipient.
- IQ handler for `urn:xmpp:mam:2`: parse `<query>` filters (`with`, `start`, `end`), execute SQLite query with RSM pagination, return results as `<forwarded>` stanzas followed by `<fin>`.
- Implement XEP-0059 RSM: `<set>`, `<first>`, `<last>`, `<count>`, `<after>`, `<before>` elements.

## Done criteria

- [ ] A new device logging in fetches recent conversation history and shows it.
