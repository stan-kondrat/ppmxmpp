# Step 13 — Service Discovery for items, plus correct features list

**Status: ✅ DONE**

## What

Extend XEP-0030 support to include the items query and update the feature list to honestly reflect what the server now supports. Clients use disco results to enable/disable UI affordances.

- Respond to `disco#items` on the server JID with an empty list (or hosted services later).
- Respond to `disco#info` on the server JID with all features now true: `jabber:iq:roster`, `urn:xmpp:carbons:2`, `urn:xmpp:ping`, `vcard-temp` (optional), plus any others.
- Respond to `disco#info` on a bare user JID with that user's features (minimal: identity `account/registered`, empty feature list).

## Specs

- **XEP-0030** — Service Discovery (full info + items).
- **XEP-0115** — Entity Capabilities (optional; advertising a caps hash on server presence reduces disco traffic from clients).

## Current state

Step 6 adds a minimal disco#info handler for the server JID. This step extends it to cover disco#items and user JID queries, and updates the feature list to match what has actually been implemented by Steps 7–12.

## What to build

- disco#items handler: `http://jabber.org/protocol/disco#items` → empty `<query/>` (no components yet).
- disco#info handler for bare user JIDs: return identity `account/registered` with empty feature list.
- Update the server disco#info feature list dynamically based on which handlers are registered (or statically list all implemented features after each step lands).

## Done criteria

- [x] Client's "server features" panel lists the right features.
- [x] Clients no longer send disco queries that the server fails to answer.
