# Step 26 — Message Delivery Receipts (XEP-0184)

## Goal

Allow senders to request acknowledgement that a message was delivered to the recipient's client.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0184 | Message Delivery Receipts |

## Overview

A sender includes `<request xmlns='urn:xmpp:receipts'/>` in a message. When the recipient's client receives the message, it sends back a `<message>` containing `<received xmlns='urn:xmpp:receipts' id='...'/>`.

The server's role is minimal: route both the original message and the receipt stanza. No server-side state is required beyond normal message routing.

## Implementation Steps

1. Ensure message routing (step-10) passes through `<request>` child elements unmodified.
2. No server-side receipt tracking is needed — the client handles sending receipts.
3. Advertise `urn:xmpp:receipts` in server disco#features so clients know the server does not strip the element.

## Test Cases

- Message with `<request/>` routed to recipient unmodified.
- Receipt `<received/>` from recipient routed back to sender unmodified.
