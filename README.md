# ppmxmpp - xmpp-server

## Features

### Core XMPP (RFC 6120)

- **STARTTLS** (RFC 6120 §5) — upgrades a plaintext connection to TLS before any authentication is attempted.
- **SASL PLAIN** (RFC 4616 + RFC 7622) — username/password authentication over the encrypted stream.
- **Resource bind** (RFC 6120 §7) — assigns a full JID (`user@domain/resource`) to each session.
- **Stream error handling** (RFC 6120 §4.9) — sends a structured `<stream:error>` element and closes the stream gracefully on protocol violations.
- **State-machine protocol ordering** — enforces the correct negotiation sequence: `CONNECTED_TCP → FEATURES_RECEIVED → STARTTLS_SENT → TLS_NEGOTIATED → FEATURES_RECEIVED_POST_TLS → SASL_SUCCESS → BOUND → ONLINE`; out-of-order stanzas are rejected.

## File Structure

```
.
├── Makefile                  # Build orchestration (third-party + server targets)
├── README.md                 # This file
├── config/
│   └── ppmxmpp.conf          # Default server configuration file
├── data/                     # Runtime data (SQLite DB, TLS certificates)
├── docs/
│   ├── plan/                 # Development plan and step-by-step design docs
│   └── specs/                # RFC/XEP specification references
├── include/
│   ├── config.h              # Config module header
│   ├── log.h                 # Logging module header
│   ├── server.h              # Server module header
│   ├── storage/              # Database and user storage headers
│   ├── tls.h                 # TLS module header
│   ├── xmpp.h                # XMPP protocol module header
│   └── xmpp_sasl.h           # SASL authentication module header
├── scripts/                  # Utility scripts
├── src/
│   ├── main.c                # Application entry point
│   ├── config.c              # Config parsing implementation
│   ├── log.c                 # Logging implementation
│   ├── server.c              # Server event loop implementation
│   ├── storage/              # Database and user storage implementations
│   ├── tls.c                 # TLS certificate/key management
│   ├── xmpp.c                # XMPP protocol handling (stream negotiation, bind)
│   └── xmpp_sasl.c           # SASL PLAIN authentication (RFC 4616 + RFC 7622)
├── test_e2e/                 # End-to-end integration tests (shell scripts)
│   ├── _common.sh            # Shared test helpers (sourced by every test)
│   ├── auth.sh               # E2E: server starts with TLS, profanity connects
│   ├── profanity_connect.sh  # E2E: profanity authenticates and reaches ONLINE state
│   ├── tls_auto_generation.sh # E2E: server auto-generates self-signed cert
│   └── tls_connection.sh     # E2E: server loads external cert, openssl s_client connects
├── tests/                    # Unit tests (C, using cmocka)
│   ├── test_config.c         # Config module unit tests
│   ├── test_db.c             # Database module unit tests
│   ├── test_server.c         # Server module unit tests
│   ├── test_users.c          # Users module unit tests
│   ├── test_xmpp_helpers.c   # Shared XMPP test helpers (mock write, DB setup, SASL feed)
│   ├── test_xmpp_helpers.h   # Shared XMPP test helpers header
│   ├── test_xmpp_sasl.c      # SASL PLAIN authentication unit tests
│   ├── test_xmpp_starttls.c  # STARTTLS negotiation unit tests
│   └── test_xmpp_state.c     # XMPP state machine unit tests (protocol ordering)
├── third_party/              # Git-submodule third-party libraries
│   ├── cmocka/               # Unit testing framework (CMake)
│   ├── libconfig/            # Configuration file parsing (CMake)
│   ├── libstrophe/           # XMPP protocol library (Autotools; requires expat/libxml2 + OpenSSL)
│   ├── libuv/                # Async I/O library (CMake)
│   ├── mbedtls/              # TLS / crypto library (CMake)
│   ├── sqlite/               # Embedded database (Custom make)
│   └── stumpless/            # Logging library (CMake)
├── docs/specs/               # RFC/XEP specs cross-checked against the implementation
│   ├── rfc4616-sasl-plain.txt  # SASL PLAIN mechanism
│   ├── rfc6120-xmpp-core.txt   # XMPP Core (RFC 6120 §5 STARTTLS, §4.9 stream errors)
│   ├── rfc7622-jid-format.txt  # XMPP JID format
│   ├── xep-0030-service-discovery.xml
│   └── xep-0199-xmpp-ping.xml
└── build/                    # Build output directories (debug/asan)
```

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
