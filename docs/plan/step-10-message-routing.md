# Step 10 — Message routing for online users

**Status: ✅ DONE**

## What

Route `<message type="chat">` between bound, available sessions on the same server. This is the moment chat starts working.

Routing rules (RFC 6121 §8):
- Full JID target (`user@host/resource`): deliver only to that exact resource if available; otherwise treat as bare JID.
- Bare JID target (`user@host`): deliver to the resource with highest priority; if multiple share the highest priority, deliver to the most recently active one. If no resource is available, hand off to the offline-message store (Step 11).
- Self-routed messages (to bare JID of self) are delivered to all of the user's resources except the sender.

## Specs

- **RFC 6121 §5** — exchanging messages (stanza shape, types).
- **RFC 6121 §8** — server rules for processing stanzas (the routing table).

## What was built

### `src/xmpp_session.c` + `include/xmpp_session.h`

Session table extracted from `xmpp_presence.c` into its own module. Owns the
`session_entry_t` array and all lookup/delivery operations.

Key API:
- `xmpp_session_table_register/unregister/write` — lifecycle and direct delivery
- `xmpp_session_table_best_resource` — priority + recency tiebreak lookup
- `xmpp_session_table_broadcast_except` — fan-out to all resources of a bare JID except one
- `xmpp_session_table_update_priority` / `xmpp_session_table_touch` — updated by presence and message handlers
- `xmpp_session_bare_jid` — shared JID utility (strip resource)

Priority and last-active tracking added to `session_entry_t`. Priority is parsed
from `<presence><priority>` in `xmpp_presence_handle`.

### `src/xmpp_message.c` + `include/xmpp_message.h`

`xmpp_message_handle()` implements RFC 6121 §8:

1. Validates `to=` present and domain matches this server (sends stanza error otherwise).
2. Extracts `<body>` text, `type`, `id` attributes.
3. Full JID target → `xmpp_session_table_write`; falls through to bare-JID routing if not found.
4. Bare JID, self-addressed → `xmpp_session_table_broadcast_except` (all own resources except sender).
5. Bare JID, other user → `xmpp_session_table_best_resource` then `xmpp_session_table_write`; silently dropped if no resource online (Step 11).
6. Touches sender's `last_active` on successful delivery.

### `src/xmpp.c`

`on_stanza()` `XMPP_STATE_ONLINE` case extended with:
```c
} else if (strcmp(sname, "message") == 0) {
    xmpp_message_handle(ctx, stanza);
}
```

### `tests/xmpp_message.c`

7 cmocka tests:
- `test_message_a_to_b_full_jid`
- `test_message_a_to_b_bare_jid`
- `test_message_bare_jid_priority_tiebreak`
- `test_message_to_self_bare_jid`
- `test_message_to_offline_user_silently_dropped`
- `test_message_missing_to_returns_error`
- `test_message_reply_b_to_a`

## Done criteria

- [x] User A sends a chat message to User B; User B's client receives it.
- [x] Reply from B to A is received.
- [x] Message to bare JID with multiple resources lands on the correct one per the priority/recency rule.
