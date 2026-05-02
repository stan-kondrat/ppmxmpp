# Step 28 — Roster Versioning (XEP-0237)

## Goal

Allow clients to request only roster changes since their last sync, reducing bandwidth on reconnect.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0237 | Roster Versioning |

## Overview

The server maintains a monotonic version token for each user's roster. On `<iq type="get" id="..."><query xmlns='jabber:iq:roster' ver='v7'/></iq>`, the server either:
- Returns the full roster if `ver` is unknown or empty.
- Returns only the delta (items added/changed/removed since `ver`) if `ver` is known.
- Returns an empty result with `ver` attribute if the roster is unchanged.

## Implementation Steps

1. Add `roster_version` column to `users` table (monotonic counter or hash).
2. Add `roster_version` column to `roster` table recording the version at which each row last changed.
3. Update roster version on every roster write (add/remove/update contact).
4. Extend roster get IQ handler (step-07) to accept and process `ver` attribute.
5. Return delta or empty result as appropriate.
6. Advertise `urn:xmpp:features:rosterver` in stream features.

## Test Cases

- First connect (no ver) → full roster returned.
- Reconnect with current ver → empty result, no data transferred.
- Reconnect after one contact added → only that delta returned.
