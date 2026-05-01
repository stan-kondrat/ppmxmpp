# Step 17 — Optional: SCRAM-SHA-256 to retire PLAIN

**Status: ❌ NOT DONE**

## What

PLAIN is fine over TLS but server-side hashing means the database never holds plaintext passwords. Add SCRAM as a server-supported mechanism alongside PLAIN; eventually advertise only SCRAM.

- Schema migration: add `salt`, `iteration_count`, `stored_key`, `server_key` columns to `users`.
- Implement the server side of the SCRAM exchange using mbedTLS primitives (HMAC-SHA-256, PBKDF2, base64).
- Channel binding (`SCRAM-SHA-256-PLUS`) is a future enhancement.

## Specs

- **RFC 5802** — SCRAM (general mechanism).
- **RFC 7677** — SCRAM-SHA-256 / SCRAM-SHA-256-PLUS profiles.
- **RFC 6120 §6** — SASL profile binding for XMPP.

## Current state

SASL PLAIN is implemented (Step 4). Passwords are stored as plaintext in the `users` table. No SCRAM infrastructure exists — no key derivation, no HMAC, no multi-round challenge/response handling.

## What to build

- Schema migration: add `salt` (TEXT), `iteration_count` (INTEGER), `stored_key` (BLOB), `server_key` (BLOB) to `users`. Migrate existing users on first login (derive keys from plaintext password, then clear the plaintext column).
- `src/xmpp_sasl_scram.c`:
  - Server-first message: generate nonce, send challenge.
  - Client-final message: verify `ClientProof` using `StoredKey`, compute and return `ServerSignature`.
  - Use `mbedtls_md_hmac()` for HMAC-SHA-256 and `mbedtls_pkcs5_pbkdf2_hmac()` for PBKDF2.
- Advertise `SCRAM-SHA-256` in the SASL mechanisms list alongside `PLAIN`.

## Done criteria

- [ ] Real client authenticates with SCRAM-SHA-256.
- [ ] Database contains no plaintext passwords for migrated users.
