# Step 24 — Entity Capabilities (XEP-0115)

## Goal

Cache client feature sets announced via `<c>` presence extensions to avoid redundant disco#info queries.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0115 | Entity Capabilities |

## Overview

Clients include a `<c xmlns='http://jabber.org/protocol/caps' node='' ver='' hash='sha-1'>` element in every presence stanza. The `ver` is a deterministic hash of the client's supported features. The server caches the feature set keyed by `(node, ver)` so it only needs to query a client once.

## Implementation Steps

1. Parse `<c>` element from incoming `<presence>` stanzas.
2. Look up `(node, ver)` in `caps_cache` table.
3. On cache miss: send `disco#info` query to the client JID; cache the response.
4. Expose cached capabilities to other server components (PEP fan-out filter, MUC, etc.).

## Data Model (SQLite)

```sql
CREATE TABLE caps_cache (
    node TEXT,
    ver TEXT,
    features_json TEXT,
    PRIMARY KEY (node, ver)
);
```

## Test Cases

- Client sends presence with `<c>` → server queries disco#info once.
- Second client with same `(node, ver)` → no new disco#info query (cache hit).
- Cached features accessible by PEP component.
