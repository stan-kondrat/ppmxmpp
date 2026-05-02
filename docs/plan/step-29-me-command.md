# Step 29 — The /me Command (XEP-0245)

## Goal

Support the `/me` action message convention by passing through the `/me` body prefix unmodified.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0245 | The /me Command |

## Overview

XEP-0245 is a purely client-side convention. A message body beginning with `/me ` is displayed by clients as a third-person action (e.g. `/me waves` → "* Alice waves"). The server has no special handling to implement — it routes the message as-is.

## Implementation Steps

1. No server-side changes are required beyond normal message routing (step-10).
2. Ensure the server does not strip or modify message `<body>` content.
3. Advertise `urn:xmpp:me-command:0` in disco#features to signal support.

## Notes

This step is intentionally minimal. The implementation is a one-liner in the disco features list.
