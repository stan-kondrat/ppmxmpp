# Step 32 — OpenPGP Encryption (XEP-0027)

## Goal

Support legacy OpenPGP-signed and encrypted messages as used by Conversations and other clients.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0027 | Current Jabber OpenPGP Usage |

## Overview

XEP-0027 is an end-to-end encryption layer. The server does **not** encrypt or decrypt content — it routes messages transparently. Encrypted messages carry a `<x xmlns='jabber:x:encrypted'>` child with the PGP-encrypted body, and signed messages carry `<x xmlns='jabber:x:signed'>` with a PGP signature of the sender's presence status.

## Implementation Steps

1. No server-side crypto is required.
2. Ensure the message router (step-10) does not strip unknown child elements from `<message>` or `<presence>` stanzas.
3. Advertise `jabber:x:encrypted` and `jabber:x:signed` in disco#features to signal passthrough support.

## Notes

XEP-0027 is superseded by XEP-0373/XEP-0374 (OpenPGP for XMPP / OX) for new deployments, but remains widely used by Conversations. Full OX support would be a separate step.

## Test Cases

- Message with `<x xmlns='jabber:x:encrypted'>` routed to recipient with element intact.
- Presence with `<x xmlns='jabber:x:signed'>` broadcast to subscribers with element intact.
