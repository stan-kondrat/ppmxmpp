# Step 6 — IQ router with ping and disco#info

**Status: ✅ DONE**

## What

Build a small IQ dispatcher keyed on `(type, top-level-child-namespace)`. Route to handlers for ping and disco#info; everything else returns `<feature-not-implemented/>`. Use `id` echo for responses.

## Specs

- **RFC 6120 §8** — stanza semantics, especially IQ rules.
- **XEP-0030 — Service Discovery** — minimal `disco#info` responder for the server JID:
  - Identity: category `server`, type `im`, name from config.
  - Features: `urn:xmpp:ping`, `http://jabber.org/protocol/disco#info`, `jabber:iq:roster`, and any others advertised.
- **XEP-0199 — XMPP Ping** — empty result IQ for `urn:xmpp:ping`.

## Implementation

- `src/xmpp_iq.c` — dispatcher keyed on `(type, child-namespace)`, wired into `on_stanza()` for `XMPP_STATE_ONLINE`.
- `include/xmpp_iq.h` — public `xmpp_iq_dispatch()` declaration.
- Ping handler (`urn:xmpp:ping` + `type=get`): empty result IQ with echoed `id`.
- Disco#info handler (`http://jabber.org/protocol/disco#info` + `type=get`): server identity + feature list; `item-not-found` for unknown entities.
- Fallback: `<feature-not-implemented/>` for unknown `get`/`set`; `result`/`error` from client silently ignored per RFC 6120 §8.2.3.
- Unit tests: `tests/test_xmpp_iq_ping.c` (3 tests), `tests/test_xmpp_iq_disco.c` (5 tests), both wired into `Makefile`.

## Checkpoint

At this point: TLS works, auth works, bind works, ping works. Client is "connected" but cannot chat yet. Steps 7–11 are about routing real conversation.

## Done criteria

- [x] Real client pings the server and gets a result.
- [x] Real client queries `disco#info` on the server JID and receives identity and feature list.
- [x] Unsupported namespace returns `<feature-not-implemented/>`.
