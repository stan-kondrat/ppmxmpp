# Step 23 — User Avatar (XEP-0084)

## Goal

Allow users to publish and retrieve profile avatars via PEP (XEP-0163).

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0084 | User Avatar |
| XEP-0163 | Personal Eventing Protocol (prerequisite — see step-25) |

## Overview

XEP-0084 uses two PEP nodes:
- `urn:xmpp:avatar:metadata` — image hash, dimensions, MIME type.
- `urn:xmpp:avatar:data` — base64-encoded image binary.

## Implementation Steps

1. Ensure PEP (step-25) is implemented first.
2. Store avatar data and metadata as PEP node items in `pep_items` table.
3. On publish to `urn:xmpp:avatar:metadata`, fan out to subscribers (contacts with caps advertising avatar support).
4. Serve avatar data on `<iq type="get">` to `urn:xmpp:avatar:data` node.

## Test Cases

- Publish avatar → metadata event fanned out to subscribers.
- Fetch avatar data by hash → returns base64 blob.
- Update avatar → new metadata event delivered.
