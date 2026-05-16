# Step 9 — Subscription handshake

**Status: ✅ DONE**

## What

Implement the presence subscription state machine so two users can become contacts.

- `<presence type="subscribe">` from A to B: deliver to B if B is online; store as pending in roster (`ask=subscribe`) if offline.
- B replies `<presence type="subscribed">` or `<presence type="unsubscribed">`.
- Update both rosters: A gets `subscription=to`, B gets `subscription=from`. If reciprocal, both become `both`.
- Generate roster pushes on each side after each transition.

## Specs

- **RFC 6121 §3** — managing presence subscriptions, including the full state table in §3.1.

## Current state

No `<presence type='subscribe/subscribed/unsubscribe/unsubscribed'>` handling. Roster subscription field exists in the schema design (Step 7) but is not yet created.

## What to build

- Presence type dispatcher: detect `subscribe`, `subscribed`, `unsubscribe`, `unsubscribed` types.
- Full RFC 6121 §3.1 state table: transitions for each combination of current subscription state and incoming stanza type.
- Pending-subscribe storage: use the roster `ask` column to track outbound subscription requests.
- Deliver subscription stanzas to online targets; queue (as roster `ask` state) for offline targets.
- Roster push to both parties after each state change.

## Done criteria

- [x] Two real clients perform "Add contact" flow, accept the request, and end up with bidirectional subscription (`both`).
- [x] After mutual subscription, both clients see each other's online presence.
