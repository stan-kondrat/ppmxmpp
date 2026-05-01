# Step 19 — Optional: File Sharing

**Status: ❌ NOT DONE**

## What

Two complementary mechanisms cover file sharing in modern XMPP:

1. **HTTP Upload (XEP-0363)** — the client uploads a file to an HTTP service, gets back a URL, and sends that URL in a chat message. The server hosts the HTTP upload slot service. This is the dominant method today.
2. **Jingle File Transfer (XEP-0234 + XEP-0166)** — peer-to-peer file transfer negotiated via Jingle. The server only relays the Jingle signalling stanzas; actual data transfer is direct between clients (or via a TURN/SOCKS5 proxy). Complex to implement.

Recommended approach: implement HTTP Upload only. It covers 95% of real-world use, requires no P2P infrastructure, and integrates with MAM naturally (the URL is in the archived message).

## Specs

### HTTP Upload (recommended)
- **XEP-0363** — HTTP File Upload.
- The server exposes a subdomain component or a path on the same host that clients can query for upload slots.
- Client sends `<request>` IQ to the upload service JID; server returns a `<slot>` with a PUT URL and a GET URL.
- Client PUTs the file directly to the URL; server stores it on disk (or object storage).
- Client pastes the GET URL into a chat message (as `<body>` or `<x-oob>`).

### Jingle File Transfer (complex, optional)
- **XEP-0166** — Jingle (session negotiation framework).
- **XEP-0234** — Jingle File Transfer (file transfer over Jingle).
- **XEP-0065** — SOCKS5 Bytestreams (fallback transport when direct P2P fails).
- **XEP-0176** — Jingle ICE-UDP Transport (for NAT traversal).
- Server role: route Jingle IQ stanzas between clients. Optionally run a SOCKS5 proxy (XEP-0065 proxy component) for clients that cannot connect directly.

## Current state

Not implemented. No HTTP upload service, no Jingle routing, no file storage. Not mentioned in any prior plan steps.

## What to build (HTTP Upload path)

- HTTP upload service, either:
  - Embedded in the XMPP server process as a libuv HTTP listener, or
  - A separate small HTTP server process (simpler, avoids mixing protocols).
- `<request>` IQ handler for namespace `urn:xmpp:http:upload:0`: validate `filename`, `size`, `content-type`; enforce size cap (e.g., 50 MB); return signed PUT/GET URLs (HMAC-based expiry or random token stored in SQLite).
- PUT handler: write file to `data/uploads/<token>/<filename>` (or similar), enforce Content-Length.
- GET handler: serve the file with correct Content-Type.
- Advertise the upload service JID in disco#items (Step 13).
- Optional: storage quota per user; expiry/cleanup of old uploads.

## Done criteria

- [ ] Client requests an upload slot and receives PUT + GET URLs.
- [ ] Client uploads a file; another client downloads it via the GET URL and sees it in the chat.
- [ ] Oversized upload request is rejected with an appropriate error.
