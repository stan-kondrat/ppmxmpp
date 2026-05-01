# Step 20 — Optional: Voice and Video Calls

**Status: ❌ NOT DONE**

## What

Voice and video calls in XMPP use **Jingle** (XEP-0166) for session negotiation and **WebRTC / ICE** for the actual media transport. The XMPP server's role is minimal: route Jingle IQ stanzas between clients and optionally run a TURN relay for clients behind symmetric NAT.

The media (RTP/SRTP audio and video) flows directly between clients — the server never touches it unless a TURN relay is needed.

## Specs

- **XEP-0166** — Jingle (session negotiation: session-initiate, session-accept, session-terminate, content-add/remove).
- **XEP-0167** — Jingle RTP Sessions (audio/video media description format).
- **XEP-0176** — Jingle ICE-UDP Transport Method (ICE candidate exchange for NAT traversal).
- **XEP-0215** — External Service Discovery (server advertises STUN/TURN server addresses to clients).
- **RFC 8829** — WebRTC: SDP semantics (clients use WebRTC internally; Jingle maps to/from SDP).
- **RFC 5766** — TURN (relay for symmetric NAT); you need a TURN server (e.g., coturn) even if the XMPP server itself doesn't implement it.

## Current state

Not implemented. No Jingle signalling routing, no TURN/STUN advertisement, no media infrastructure.

## Architecture

```
Client A ──Jingle IQ──► XMPP server ──Jingle IQ──► Client B
Client A ◄──────────── RTP/SRTP ────────────────► Client B
                (direct, or via TURN relay)
```

The XMPP server only routes the Jingle IQ stanzas (already handled generically by the IQ router in Step 6 and message routing in Step 10 — Jingle stanzas are addressed to the remote client JID). No special server-side Jingle logic is required for the happy path.

## What the server must actually implement

1. **XEP-0215 — External Service Discovery**: respond to `<services xmlns='urn:xmpp:extdisco:2'/>` IQ with the STUN/TURN server hostname, port, and optionally short-lived TURN credentials (HMAC-SHA1 over timestamp + username).
2. **Advertise Jingle features in disco#info** (Step 13): `urn:xmpp:jingle:1`, `urn:xmpp:jingle:apps:rtp:1`, `urn:xmpp:jingle:transports:ice-udp:1`.
3. Route Jingle `<iq>` stanzas correctly between full JIDs — this is already handled by Steps 6 and 10 if they route IQs to remote JIDs.

## External dependencies

- A **STUN server** (e.g., the public Google STUN server, or self-hosted `coturn --stun-only`).
- A **TURN server** (e.g., `coturn`) for clients behind symmetric NAT. The XMPP server issues short-lived TURN credentials via XEP-0215.
- The TURN server is a separate process; the XMPP server only advertises its address and generates credentials.

## Done criteria

- [ ] Server responds to XEP-0215 `<services/>` query with STUN/TURN address and credentials.
- [ ] Two clients on the same LAN can complete a Jingle audio call (ICE direct path).
- [ ] Two clients behind NAT can complete a Jingle audio call via the TURN relay.
- [ ] Video call works between two clients (same as audio, additional video codec negotiation).
