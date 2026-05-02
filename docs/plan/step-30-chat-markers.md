# Step 30 — Chat Markers (XEP-0333)

## Goal

Allow clients to signal message receipt, display, and acknowledgement states to other participants.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0333 | Chat Markers |

## Overview

Clients send `<message>` stanzas containing one of:
- `<received xmlns='urn:xmpp:chat-markers:0' id='...'/>` — message received by client.
- `<displayed .../>` — message shown to user.
- `<acknowledged .../>` — user has acted on the message.

The server routes these marker messages like any other message. Optionally, MAM (step-16) archives them.

## Implementation Steps

1. No special routing logic needed — chat marker messages are standard `<message>` stanzas.
2. Ensure MAM (step-16) archives chat marker messages (they have a `<body>`-less `<message>` form).
3. Advertise `urn:xmpp:chat-markers:0` in disco#features.

## Test Cases

- `<displayed>` marker sent by recipient → routed to sender unmodified.
- Marker messages archived by MAM when applicable.
