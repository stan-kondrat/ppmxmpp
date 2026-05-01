# Step 1 — Decouple libstrophe from OpenSSL

**Status: ✅ DONE**

## What

Reconfigure the libstrophe submodule build to disable its TLS layer entirely. Use libstrophe only as a parser and stanza serializer; keep TLS in `src/tls.c` via mbedTLS.

## Why this is first

Until libstrophe stops pulling in OpenSSL, you have two TLS stacks in one binary. Every later phase that produces or parses stanzas will inherit the wrong link line.

## Specs

- None directly. Build hygiene only.

## Current state

libstrophe is built via Autotools with `--disable-tls`, which completely decouples it from OpenSSL. It is used exclusively via its internal expat-based XML parser API (private headers from `third_party/libstrophe/src/`). `tls.c` uses mbedTLS PSA Crypto API exclusively — no OpenSSL anywhere in the build.

## Done criteria

- [x] `nm` on `libstrophe.a` shows zero `SSL_*`, `EVP_*`, or `gnutls_*` symbols.
- [x] Server binary has no `libssl` or `libcrypto` dependency.
- [x] All existing e2e TLS tests still pass.
