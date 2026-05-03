# ppmxmpp - xmpp-server

## Features

### Core XMPP (RFC 6120)

- **STARTTLS** (RFC 6120 §5) — upgrades a plaintext connection to TLS before any authentication is attempted.
- **SASL PLAIN** (RFC 4616 + RFC 7622) — username/password authentication over the encrypted stream.
- **Resource bind** (RFC 6120 §7) — assigns a full JID (`user@domain/resource`) to each session.
- **Stream error handling** (RFC 6120 §4.9) — sends a structured `<stream:error>` element and closes the stream gracefully on protocol violations.
- **State-machine protocol ordering** — enforces the correct negotiation sequence: `CONNECTED_TCP → FEATURES_RECEIVED → STARTTLS_SENT → TLS_NEGOTIATED → FEATURES_RECEIVED_POST_TLS → SASL_SUCCESS → BOUND → ONLINE`; out-of-order stanzas are rejected.

## Dependencies

### Build tools

- `cmake`
- `make`
- `gcc` or `clang`
- `autoconf`, `automake`, `libtool` — required by libstrophe (Autotools-based)
- `python3-jsonschema` — required by mbedtls to generate PSA crypto driver wrappers

Initialize submodules after cloning:
```
git submodule update --init --recursive
```

## Building

```
make              # debug build (default)
make BUILD=release
make BUILD=asan
```

Build individual third-party libraries:
```
make mbedtls
make libuv
make libstrophe
make sqlite
make stumpless
make cmocka
make libconfig
```

Override static/shared per library (default: static=YES, shared=NO):
```
make third-party LIBUV_SHARED=YES SQLITE_SHARED=YES
```

## Testing

Run unit tests (C, cmocka):
```
make test
```

Run end-to-end tests (shell scripts):
```
make test-e2e
```

## Configuration

The default config file is `config/ppmxmpp.conf`. It is created with defaults on first run if it does not exist. A different path can be passed with `--config <file>`.

Command-line arguments always override values from the config file.

### Cleaning

```
make clean
```
