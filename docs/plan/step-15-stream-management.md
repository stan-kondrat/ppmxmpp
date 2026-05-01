# Step 15 — Stream Management (strongly recommended)

**Status: ❌ NOT DONE**

## What

Mobile clients drop and reconnect constantly. Without stream management, every reconnect drops messages. With it, a client can resume a session and the server replays unacked stanzas.

- Advertise `<sm xmlns="urn:xmpp:sm:3"/>` in features after bind.
- Handle `<enable/>`: generate session id, track outbound stanzas with sequence numbers, accept `<a/>` acks and respond to `<r/>` requests.
- Handle `<resume previd="..." h="..."/>`: rebind to the prior session, replay unacked outbound stanzas.

## Specs

- **XEP-0198** — Stream Management.

## Current state

Not implemented. Features list does not advertise `urn:xmpp:sm:3`. No session resumption infrastructure.

## What to build

- Outbound stanza queue per session (ring buffer or linked list) with sequence counter.
- `<enable/>` handler: generate a random session ID, start tracking.
- `<a h="..."/>` handler: discard acked stanzas from the queue.
- `<r/>` handler: send `<a h="..."/>` with the current inbound counter.
- `<resume>` handler: look up the session by `previd`, verify `h`, replay unacked stanzas on the new connection.
- Session store: keep stanza queue alive for a configurable timeout after disconnect (e.g., 5 minutes) to allow resumption.

## Checkpoint

After Step 15 the server is genuinely usable as a small private chat server. Steps 16–17 are quality-of-life.

## Done criteria

- [ ] Mobile client switches networks; server replays in-flight messages; user sees no gap.
