# Step 8 — Presence routing within the server

**Status: ✅ DONE**

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

- [x] Two clients of the same user log in; each sees the other's presence.
- [x] A user's presence appears as "online" on a contact's roster after mutual subscription.
- [x] Disconnecting a resource sends `unavailable` to subscribers.

## Implementation notes

- `include/xmpp_presence.h` / `src/xmpp_presence.c`: flat array session registry (`SESSION_TABLE_CAP 256`), swap-with-last unregister, `broadcast_presence` opens DB per broadcast via `storage_roster_list`.
- Session registration deferred to initial `<presence/>` send (not bind), per RFC 6121 §4.2.
- `xmpp_presence_on_disconnect` is a no-op if the session never sent initial presence (no unavailable flood for connections that dropped before going available).
- `src/xmpp.c`: ONLINE state dispatches `presence` stanzas to `xmpp_presence_handle`.
- `src/server.c` `on_conn_close`: calls `xmpp_presence_on_disconnect` before `xmpp_session_cleanup`.
- 14 unit tests in `tests/xmpp_presence.c` covering registry, initial/unavailable/directed presence, and disconnect.
