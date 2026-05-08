# File Structure

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
│   ├── xmpp_iq.h             # IQ dispatch module header
│   ├── xmpp_iq_buf.h         # Inline helpers: iq_append / iq_flush (shared by IQ handlers)
│   ├── xmpp_sasl.h           # SASL authentication module header (sasl_rc_t + handle_sasl_plain)
│   ├── xep-0030-service-discovery.h  # XEP-0030 disco#info handler header
│   └── xep-0199-ping.h       # XEP-0199 ping handler header
├── scripts/                  # Utility scripts
├── src/
│   ├── main.c                # Application entry point
│   ├── config.c              # Config parsing implementation
│   ├── log.c                 # Logging implementation
│   ├── server.c              # Server event loop implementation
│   ├── storage/              # Database and user storage implementations
│   ├── tls.c                 # TLS certificate/key management
│   ├── xmpp.c                # XMPP protocol handling (stream negotiation, bind)
│   ├── xmpp_iq.c             # IQ stanza dispatch (roster, XEP-0030, XEP-0199, errors)
│   ├── xmpp_sasl.c           # SASL PLAIN authentication (RFC 4616 + RFC 7622)
│   ├── xep-0030-service-discovery.c  # XEP-0030: disco#info handler
│   └── xep-0199-ping.c       # XEP-0199: ping handler
├── test_e2e/                 # End-to-end integration tests (shell scripts)
│   ├── _common.sh            # Shared test helpers (sourced by every test)
│   ├── auth.sh               # E2E: server starts with TLS, profanity connects
│   ├── profanity_bind.sh     # E2E: profanity binds a resource; verifies full JID in result
│   ├── profanity_connect.sh  # E2E: profanity authenticates and reaches ONLINE state
│   ├── sasl_auth_failure_cap.sh # E2E: three bad passwords close stream with <policy-violation/>
│   ├── tls_auto_generation.sh # E2E: server auto-generates self-signed cert
│   └── tls_connection.sh     # E2E: server loads external cert, openssl s_client connects
├── tests/                    # Unit tests (C, using cmocka)
│   ├── test_config.c         # Config module unit tests
│   ├── test_db.c             # Database module unit tests
│   ├── test_server.c         # Server module unit tests
│   ├── test_users.c          # Users module unit tests
│   ├── test_xmpp_helpers.c   # Shared XMPP test helpers (mock write, DB setup, SASL feed, feed_to_online)
│   ├── test_xmpp_helpers.h   # Shared XMPP test helpers header
│   ├── test_xmpp_roster.c    # Roster IQ unit tests (get/set/remove)
│   ├── test_xmpp_sasl.c      # SASL PLAIN authentication unit tests
│   ├── test_xmpp_starttls.c  # STARTTLS negotiation unit tests
│   ├── test_xmpp_state.c     # XMPP state machine unit tests (protocol ordering)
│   ├── xep-0030-service-discovery.c  # XEP-0030 disco#info integration tests
│   └── xep-0199-ping.c       # XEP-0199 ping integration tests
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
│   ├── rfc6121-xmpp.txt        # XMPP Instant Messaging (roster, presence)
│   ├── rfc7622-jid-format.txt  # XMPP JID format
│   ├── xep-0030.xml            # XEP-0030 (alternate copy)
│   ├── xep-0030-service-discovery.xml  # XEP-0030 disco#info / disco#items
│   └── xep-0199-xmpp-ping.xml  # XEP-0199 XMPP Ping
└── build/                    # Build output directories (debug/asan)
```
