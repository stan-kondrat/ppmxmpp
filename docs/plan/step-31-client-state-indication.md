# Step 31 — Client State Indication (XEP-0352)

## Goal

Allow clients to signal whether they are active or inactive (backgrounded), enabling the server to optimize traffic delivery.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0352 | Client State Indication |

## Overview

Clients send `<active xmlns='urn:xmpp:csi:0'/>` or `<inactive .../>` non-stanza elements. When a session is inactive the server may queue or discard low-priority traffic (e.g. presence updates from contacts not in the user's roster, typing notifications) and flush on `<active/>`.

## Implementation Steps

1. Parse `<active>` and `<inactive>` elements at stream level (not IQ/message/presence stanzas).
2. Track `csi_state` (active/inactive) per session.
3. When inactive: buffer or drop low-priority stanzas (presence broadcasts, non-urgent PEP events).
4. On `<active>`: flush buffered stanzas.
5. Advertise `urn:xmpp:csi:0` in stream features.

## Priority Traffic (always delivered)

- `<message>` stanzas.
- Subscription presence (`subscribe`, `subscribed`, etc.).
- IQ stanzas.

## Deferrable Traffic (hold when inactive)

- Regular `<presence>` updates (available/unavailable with status/show changes).
- PEP events (avatars, mood, etc.).

## Test Cases

- Send `<inactive>` → subsequent presence broadcasts from contacts buffered.
- Send `<active>` → buffered stanzas delivered.
- Messages always delivered regardless of CSI state.
