# Step 6 — IQ router with ping and disco#info

**Status: ❌ NOT DONE**

## What

Build a small IQ dispatcher keyed on `(type, top-level-child-namespace)`. Route to handlers for ping and disco#info; everything else returns `<feature-not-implemented/>`. Use `id` echo for responses.

## Specs

- **RFC 6120 §8** — stanza semantics, especially IQ rules.
- **XEP-0030 — Service Discovery** — minimal `disco#info` responder for the server JID:
  - Identity: category `server`, type `im`, name from config.
  - Features: `urn:xmpp:ping`, `http://jabber.org/protocol/disco#info`, `jabber:iq:roster`, and any others advertised.
- **XEP-0199 — XMPP Ping** — empty result IQ for `urn:xmpp:ping`.

## Current state

No IQ router exists. After binding, subsequent IQ stanzas fall through to the `default:` branch in `on_stanza()` in `src/xmpp.c` and are silently ignored. No ping or disco#info handler.

## What to build

- `src/xmpp_iq.c` — dispatcher keyed on `(type, child-namespace)`.
- Ping handler: `urn:xmpp:ping` + `type=get` → empty result IQ with echoed `id`.
- Disco#info handler: `http://jabber.org/protocol/disco#info` + `type=get` → server identity + feature list.
- Fallback: return `<iq type='error'>` with `<feature-not-implemented/>` for unknown namespaces.
- Wire the dispatcher into `on_stanza()` for the `RESOURCE_BOUND` state.

## Checkpoint

At this point: TLS works, auth works, bind works, ping works. Client is "connected" but cannot chat yet. Steps 7–11 are about routing real conversation.

## Done criteria

- [ ] Real client pings the server and gets a result.
- [ ] Real client queries `disco#info` on the server JID and receives identity and feature list.
- [ ] Unsupported namespace returns `<feature-not-implemented/>`.
