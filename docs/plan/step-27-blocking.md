# Step 27 — Blocking Command (XEP-0191)

## Goal

Allow users to block and unblock JIDs at the server level so stanzas from blocked JIDs are silently dropped.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0191 | Blocking Command |

## Overview

Clients send `<iq type="set">` with `<block>` / `<unblock>` elements containing one or more `<item jid='...'>` children. The server maintains a block list per user and enforces it on inbound stanzas.

## Implementation Steps

1. Add `block_list` table: `user_jid`, `blocked_jid`.
2. Handle `<block>` IQ: insert rows, push updated list to all user sessions, return `<iq type="result">`.
3. Handle `<unblock>` IQ: delete rows, push updated list, return result.
4. Handle `<blocklist>` get IQ: return current block list.
5. On stanza routing (steps 8/10), check sender bare JID against recipient's block list and drop if matched.
6. Advertise `urn:xmpp:blocking` in disco#features.

## Data Model (SQLite)

```sql
CREATE TABLE block_list (
    user_jid TEXT,
    blocked_jid TEXT,
    PRIMARY KEY (user_jid, blocked_jid)
);
```

## Test Cases

- Block a JID → subsequent messages from that JID not delivered.
- Unblock → messages delivered again.
- Fetch blocklist → returns all blocked JIDs.
- Block list pushed to all active sessions on change.
