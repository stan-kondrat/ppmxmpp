# Step 14 — Optional: vCard for avatars and display names

**Status: ❌ NOT DONE**

## What

Most clients show "(no name)" until they can fetch a vCard. A minimal vCard responder is enough.

- Storage: `vcards` table (owner_bare_jid TEXT PRIMARY KEY, xml TEXT).
- `<iq type="get"><vCard xmlns="vcard-temp"/></iq>` to a bare JID returns the stored vCard or an empty one.
- `<iq type="set"><vCard xmlns="vcard-temp">...</vCard></iq>` from the owner stores it.

## Specs

- **XEP-0054** — vcard-temp.
- Avatars proper use **XEP-0153** (vCard-Based Avatars) or **XEP-0084** (PEP avatars); either can be deferred.

## Current state

No vCard storage or handler. SQLite schema has no `vcards` table.

## What to build

- Schema migration: create `vcards` table.
- `src/storage/vcard.c` — get and set functions.
- IQ handler for `vcard-temp` namespace, wired into the IQ router (Step 6).
- Return an empty `<vCard/>` when no vCard is stored for the requested JID.
- Restrict set to the authenticated user's own JID.

## Done criteria

- [ ] Client sets a display name and avatar; another client fetches and sees them.
