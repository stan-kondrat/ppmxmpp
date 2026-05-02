# Step 25 — Personal Eventing Protocol (XEP-0163)

## Goal

Implement PEP as a profile of XEP-0060 (pubsub) scoped to user bare JIDs, enabling features such as user avatar (XEP-0084) and OMEMO device lists (XEP-0384).

## XEPs

| XEP | Title |
|-----|-------|
| XEP-0163 | Personal Eventing Protocol |

## Overview

PEP creates an implicit pubsub service at the user's bare JID. Each user can publish items to named nodes. Subscribers (contacts) receive event notifications automatically when the publisher is online.

Key PEP simplifications over full pubsub:
- One publisher per node (the node owner).
- Subscription is implicit for contacts with mutual presence subscription.
- Default access model: presence.

## Implementation Steps

1. Add IQ handler for `http://jabber.org/protocol/pubsub` namespace on user bare JIDs.
2. Implement `publish` — store item in `pep_items`, fan out to eligible subscribers.
3. Implement `items` (retrieve) and `retract`.
4. On user login, re-broadcast last-published item for each node to contacts (if they support the feature via caps — step-24).
5. Advertise `http://jabber.org/protocol/pubsub#publish` in disco#features.

## Data Model (SQLite)

```sql
CREATE TABLE pep_nodes (
    owner_jid TEXT,
    node TEXT,
    config_json TEXT,
    PRIMARY KEY (owner_jid, node)
);

CREATE TABLE pep_items (
    owner_jid TEXT,
    node TEXT,
    item_id TEXT,
    payload TEXT,
    PRIMARY KEY (owner_jid, node, item_id)
);
```

## Test Cases

- Publish item → subscribers receive `<message>` event.
- Retrieve items → returns stored payload.
- Login → last-published items re-broadcast to online contacts.
