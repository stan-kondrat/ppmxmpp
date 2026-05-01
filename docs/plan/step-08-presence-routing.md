# Step 8 — Presence routing within the server

**Status: ❌ NOT DONE**

## What

Handle initial presence, directed presence, and presence broadcast.

- Track each authenticated, bound resource as an "available" session in memory keyed by full JID.
- On client's initial `<presence/>`: mark resource available, broadcast presence to every contact in the roster whose subscription is `from` or `both`.
- On `<presence type="unavailable"/>` or disconnect: broadcast unavailable presence to the same set.
- On directed presence (`to=` set): route to the target resource(s).
- Presence probes from another resource of the same user: respond with current presence of all available resources.

## Specs

- **RFC 6121 §4** — exchanging presence information.
- **RFC 6121 §3** — presence subscription semantics (full subscription state machine is Step 9).

## Current state

No presence stanza handling. No in-memory session registry. No broadcast logic. There is no shared data structure tracking which connections are currently authenticated and bound.

## What to build

- In-memory session table (e.g., a hash map keyed by full JID → `conn_t*`), protected by the libuv event loop (single-threaded, so no locking needed).
- Register each connection in the table on successful resource bind (Step 5).
- Remove each connection from the table on disconnect or `<presence type="unavailable"/>`.
- `<presence/>` handler: look up roster contacts, send presence stanza to each available resource.
- Directed presence (`to=` attribute set): look up the target full or bare JID in the session table and deliver.

## Done criteria

- [ ] Two clients of the same user log in; each sees the other's presence.
- [ ] A user's presence appears as "online" on a contact's roster after mutual subscription.
- [ ] Disconnecting a resource sends `unavailable` to subscribers.
