# Step 21 — Multi-User Chat (XEP-0045, XEP-0249)

## Goal

Implement server-side Multi-User Chat rooms and direct MUC invitations.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0045 | Multi-User Chat |
| XEP-0249 | Direct MUC Invitations |

## Overview

A MUC component lives at a subdomain (e.g. `conference.example.com`). Clients join rooms via `<presence to="room@conference/nick">`, send `<message type="groupchat">`, and leave with `<presence type="unavailable">`.

## Key Concepts

- **Room JID**: `roomname@conference.example.com`
- **Occupant JID**: `room@conference/nick`
- **Affiliation**: owner / admin / member / outcast / none
- **Role**: moderator / participant / visitor

## Implementation Steps

1. Register MUC component subdomain in server config.
2. Add `rooms` table: `room_jid`, `subject`, `config_json`.
3. Add `room_members` table: `room_jid`, `user_jid`, `affiliation`, `role`.
4. Handle `<presence>` join/leave stanzas — broadcast occupant list to room.
5. Route `<message type="groupchat">` to all room occupants.
6. Implement IQ `disco#info` / `disco#items` for room listing (links to step-13).
7. Implement room creation flow: owner sends presence → server creates room → owner must configure or accept defaults.
8. XEP-0249: accept `<message>` with `<x xmlns='jabber:x:conference'>` and route to invitee.

## Data Model (SQLite)

```sql
CREATE TABLE rooms (
    jid TEXT PRIMARY KEY,
    subject TEXT,
    config JSON
);

CREATE TABLE room_occupants (
    room_jid TEXT,
    user_jid TEXT,
    nick TEXT,
    affiliation TEXT DEFAULT 'none',
    role TEXT DEFAULT 'participant',
    PRIMARY KEY (room_jid, user_jid)
);
```

## Test Cases

- Join room → receive occupant list presence stanzas.
- Send groupchat message → all occupants receive it.
- Leave room → occupants notified.
- Direct invite via XEP-0249 → invitee receives message.
