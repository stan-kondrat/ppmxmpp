# Step 7 — Roster storage and management

**Status: ✅ DONE**

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

- [x] Client fetches an empty roster on first login without error.
- [x] Client adds a contact; server stores it, pushes the update, client shows the contact.
- [x] Client removes a contact; server removes it, client reflects the change.

## What was built

- `src/storage/db.c` — version 2 migration creates `roster` and `roster_groups` tables with FK cascade delete.
- `include/storage/roster.h` + `src/storage/roster.c` — CRUD: `storage_roster_list`, `storage_roster_get`, `storage_roster_upsert`, `storage_roster_remove`, `storage_roster_get_groups`.
- `include/xmpp_iq.h` + `src/xmpp_iq.c` — IQ dispatcher handling `jabber:iq:roster` get and set (add/update/remove via `subscription='remove'`), wired into `xmpp.c` at `XMPP_STATE_CONNECTED`.
- Roster push: after any set/remove, server sends `<iq type='set'>` roster update to the active resource.
- `tests/test_xmpp_roster.c` — 8 unit tests covering get (empty, with items), set (add, update, remove, with group), error cases, and roster push.
