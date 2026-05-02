# Step 22 — Bookmarks (XEP-0048)

## Goal

Store and retrieve per-user conference room bookmarks using private XML storage.

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0048 | Bookmarks |

## Overview

Clients store bookmarks as a `<storage xmlns='storage:bookmarks'>` element in private XML storage (XEP-0049). Each `<conference>` child element represents a saved MUC room, with optional autojoin flag and preferred nick.

## Implementation Steps

1. Implement XEP-0049 Private XML Storage IQ handler (`jabber:iq:private`).
2. Persist private XML blobs per user in SQLite (`user_private_xml` table).
3. On client connect, server returns stored bookmarks in response to `<iq type="get">`.
4. Client sets bookmarks with `<iq type="set">` — overwrite entire bookmark blob.

## Data Model (SQLite)

```sql
CREATE TABLE user_private_xml (
    user_jid TEXT,
    namespace TEXT,
    xml_data TEXT,
    PRIMARY KEY (user_jid, namespace)
);
```

## Test Cases

- Set bookmarks → stored in DB.
- Get bookmarks → returns previously stored blob.
- Multiple namespaces stored independently per user.
