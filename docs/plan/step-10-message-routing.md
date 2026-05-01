# Step 10 — Message routing for online users

**Status: ❌ NOT DONE**

## What

Route `<message type="chat">` between bound, available sessions on the same server. This is the moment chat starts working.

Routing rules (RFC 6121 §8):
- Full JID target (`user@host/resource`): deliver only to that exact resource if available; otherwise treat as bare JID.
- Bare JID target (`user@host`): deliver to the resource with highest priority; if multiple share the highest priority, deliver to the most recently active one. If no resource is available, hand off to the offline-message store (Step 11).
- Self-routed messages (to bare JID of self) are delivered to all of the user's resources except the sender.

## Specs

- **RFC 6121 §5** — exchanging messages (stanza shape, types).
- **RFC 6121 §8** — server rules for processing stanzas (the routing table).

## Current state

No `<message>` stanza handler or routing between connections. Requires the session table from Step 8 and the full bound JID from Step 5.

## What to build

- Message stanza handler wired into `on_stanza()` for the `RESOURCE_BOUND` state.
- JID parser: split `to` attribute into localpart, domain, optional resource.
- Session table lookup (from Step 8) to find the target `conn_t`.
- Priority/recency tie-breaking when multiple resources are available.
- Hand-off to offline store (Step 11) when no resource is available.

## Checkpoint

At this point two users can chat in real time. Steps 11 onward make messaging reliable and complete.

## Done criteria

- [ ] User A sends a chat message to User B; User B's client receives it.
- [ ] Reply from B to A is received.
- [ ] Message to bare JID with multiple resources lands on the correct one per the priority/recency rule.
