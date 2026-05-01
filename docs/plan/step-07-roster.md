# Step 7 — Roster storage and management

**Status: ❌ NOT DONE**

## What

A real client refuses to look like a chat app without a roster. Implement the roster:

- SQLite tables: `roster` (owner_jid, contact_jid, name, subscription, ask) and `roster_groups` (owner_jid, contact_jid, group_name).
- `<iq type="get"><query xmlns="jabber:iq:roster"/></iq>` → return the user's roster.
- `<iq type="set"><query xmlns="jabber:iq:roster"><item .../></query></iq>` → add/update/remove items.
- Roster pushes to all of the user's resources after any change.

## Specs

- **RFC 6121 §2** — roster management.
- **RFC 6121 §2.6** — roster versioning (optional but cheap; advertise `urn:xmpp:features:rosterver` and accept `ver=` attribute).

## Current state

No roster table in SQLite. No `jabber:iq:roster` handler. The schema in `storage/db.c` only has the `users` table (version 1 migration).

## What to build

- Schema migration (version 2): create `roster` and `roster_groups` tables.
- `src/storage/roster.c` — CRUD functions: get item, list items, upsert item, remove item.
- IQ handler for `jabber:iq:roster` get and set, wired into the IQ router from Step 6.
- Roster push: after any set/remove, push an `<iq type='set'>` roster update to every active resource of the owner.

## Done criteria

- [ ] Client fetches an empty roster on first login without error.
- [ ] Client adds a contact; server stores it, pushes the update, client shows the contact.
- [ ] Client removes a contact; server removes it, client reflects the change.
