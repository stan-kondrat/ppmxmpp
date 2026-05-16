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
│   ├── storage/              # Database and storage headers
│   │   ├── db.h              # SQLite DB abstraction header
│   │   ├── db_offline.h      # Offline message storage header (XEP-0160, XEP-0203)
│   │   ├── db_roster.h      # Roster storage header
│   │   └── db_users.h       # User storage header
│   ├── tls.h                 # TLS module header
│   ├── xmpp.h                # XMPP protocol module header
│   ├── xmpp_iq.h             # IQ dispatch module header
│   ├── xmpp_iq_buf.h         # Inline helpers: iq_append / iq_flush (shared by IQ handlers)
│   ├── xmpp_message.h        # Message routing module header
│   ├── xmpp_presence.h       # Presence routing module header (session registry + handler)
│   ├── xmpp_sasl.h           # SASL authentication module header (sasl_rc_t + handle_sasl_plain)
│   ├── xmpp_session.h        # XMPP session state header
│   ├── xep-0030-service-discovery.h  # XEP-0030 disco#info handler header
│   ├── xep-0160-offline-messages.h  # XEP-0160 offline message storage handler header
│   └── xep-0199-ping.h       # XEP-0199 ping handler header
├── scripts/                  # Utility scripts
├── src/
│   ├── main.c                # Application entry point
│   ├── config.c              # Config parsing implementation
│   ├── log.c                 # Logging implementation
│   ├── server.c              # Server event loop implementation
│   ├── storage/              # Database and storage implementations
│   │   ├── db.c              # SQLite DB abstraction (migrations, prepared statements)
│   │   ├── db_offline.c      # Offline message storage (XEP-0160, XEP-0203)
│   │   ├── db_roster.c       # Roster storage
│   │   └── db_users.c        # User storage
│   ├── tls.c                 # TLS certificate/key management
│   ├── xmpp.c                # XMPP protocol handling (stream negotiation, bind)
│   ├── xmpp_iq.c             # IQ stanza dispatch (roster, XEP-0030, XEP-0199, errors)
│   ├── xmpp_message.c        # Message routing implementation
│   ├── xmpp_presence.c       # Presence routing (RFC 6121 §4.2/§4.4/§4.6; session registry)
│   ├── xmpp_sasl.c           # SASL PLAIN authentication (RFC 4616 + RFC 7622)
│   ├── xmpp_session.c        # XMPP session state management
│   ├── xep-0030-service-discovery.c  # XEP-0030: disco#info handler
│   ├── xep-0160-offline-messages.c  # XEP-0160: offline message store + cap error
│   └── xep-0199-ping.c       # XEP-0199: ping handler
├── test_e2e/                 # End-to-end integration tests (shell scripts)
│   ├── _common.sh            # Shared test helpers (sourced by every test)
│   ├── _helpers_profanity.sh # Helpers for profanity-based E2E tests
│   ├── _helpers_xmpp-message.sh # Helpers for xmpp-message-based E2E tests
│   ├── _helpers_xmppc.sh     # Helpers for xmppc-based E2E tests
│   ├── auth.sh               # E2E: server starts with TLS, profanity connects
│   ├── message_routing_profanity.sh # E2E: message routing with profanity
│   ├── message_routing_xmppc.sh # E2E: message routing (bare/full JID, 3-user bidirectional) with xmppc
│   ├── offline_messages_profanity.sh # E2E: offline message delivery with profanity
│   ├── offline_messages_xmpp-message.sh # E2E: offline message delivery with xmpp-message
│   ├── offline_messages_xmppc.sh # E2E: offline message delivery with xmppc
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
│   ├── test_offline.c        # Offline message storage unit tests (XEP-0160)
│   ├── test_xmpp_helpers.c   # Shared XMPP test helpers (mock write, DB setup, SASL feed, feed_to_online)
│   ├── test_xmpp_helpers.h   # Shared XMPP test helpers header
│   ├── test_xmpp_roster.c    # Roster IQ unit tests (get/set/remove)
│   ├── test_xmpp_sasl.c      # SASL PLAIN authentication unit tests
│   ├── test_xmpp_starttls.c  # STARTTLS negotiation unit tests
│   ├── test_xmpp_state.c     # XMPP state machine unit tests (protocol ordering)
│   ├── xep-0030-service-discovery.c  # XEP-0030 disco#info integration tests
│   ├── xep-0199-ping.c       # XEP-0199 ping integration tests
│   ├── xmpp_message.c        # Message routing unit tests
│   └── xmpp_presence.c       # Presence routing unit tests (RFC 6121 §4.2/§4.4/§4.6)
├── third_party/              # Git-submodule third-party libraries
│   ├── cmocka/               # Unit testing framework (CMake)
│   ├── libconfig/            # Configuration file parsing (CMake)
│   ├── libstrophe/           # XMPP protocol library (Autotools; requires expat/libxml2 + OpenSSL)
│   ├── libuv/                # Async I/O library (CMake)
│   ├── mbedtls/              # TLS / crypto library (CMake)
│   ├── sqlite/               # Embedded database (Custom make)
│   └── stumpless/            # Logging library (CMake)
├── docs/specs/               # RFC/XEP specs referenced by implementation
│   ├── rfc4616-sasl-plain.txt         # SASL PLAIN mechanism
│   ├── rfc5766-stun-turn.txt          # STUN/TURN relay support
│   ├── rfc5802-scram.txt              # SCRAM authentication mechanism
│   ├── rfc6120-xmpp-core.txt          # XMPP Core (stream negotiation, TLS, SASL)
│   ├── rfc6121-xmpp-im.txt             # XMPP IM (roster, presence, stanzas)
│   ├── rfc7622-jid-format.txt         # XMPP JID format (RFC 7622)
│   ├── rfc7677-scram-sha256.txt       # SCRAM-SHA-256 mechanism
│   ├── rfc8155-websocket.txt          # WebSocket binding for XMPP
│   ├── rfc8553-tcp.txt                # TCP binding for XMPP
│   ├── rfc8656-turn.txt               # TURN relay protocol
│   ├── rfc8829-websocket.txt          # WebSocket transport for XMPP
│   ├── rfc9266-cbOR.txt               # Channel Binding OTP
│   ├── xep-0027-gpg-sign.xml           # GPG signed XMPP stanzas
│   ├── xep-0030-caps.xml               # Common Alerting Protocol
│   ├── xep-0045-muc.xml                # Multi-User Chat
│   ├── xep-0048-bookmarks.xml         # Bookmarks
│   ├── xep-0049-pprivate.xml           # Private XML Storage
│   ├── xep-0054-vcard-temp.xml         # vCard Temporary Protocol
│   ├── xep-0059-roster-ver.xml        # Roster Versioning
│   ├── xep-0060-pubsub.xml            # Publish-Subscribe
│   ├── xep-0065-s5b.xml               # SOCKS5 Bytestreams
│   ├── xep-0084-user-avatar.xml       # User Avatar
│   ├── xep-0115-entity-cap.xml        # Entity Capabilities
│   ├── xep-0153-user-nick.xml         # User Nickname
│   ├── xep-0160-presence-xml.xml      # Presence (XML content)
│   ├── xep-0163-pep.xml               # Personal Eventing Protocol
│   ├── xep-0166-disco-info.xml        # Service Discovery Info
│   ├── xep-0167-multi-chat.xml        # Multi-Party Messaging
│   ├── xep-0176-ignore.xml            # Ignoring Messages
│   ├── xep-0184-receipts.xml          # Message Delivery Receipts
│   ├── xep-0191-ping.xml              # Ping over XMPP
│   ├── xep-0198-stream-mgmt.xml        # Stream Management
│   ├── xep-0199-ping.xml              # XMPP Ping
│   ├── xep-0203-time.xml              # Time
│   ├── xep-0215-enc-keys.xml          # Encryption Keys
│   ├── xep-0234-omemo.xml             # OMEMO Encryption
│   ├── xep-0237-ft.xml                # File Transfer
│   ├── xep-0245-leave-muc.xml         # Leaving a MUC Room
│   ├── xep-0249-mini-presence.xml    # Minimal Presence
│   ├── xep-0280-carbons.xml           # Message Carbons
│   ├── xep-0297-hints.xml             # Stanza Hints
│   ├── xep-0313-mam.xml               # Message Archive Management
│   ├── xep-0333-spoiler.xml           # Spoiler Messages
│   ├── xep-0352-csi.xml               # Client State Indication
│   ├── xep-0363-header.xml            # HTTP TLS POI
│   ├── xep-0373-ox-encrypt.xml        # OpenPGP Encryption
│   ├── xep-0374-ox-sign.xml           # OpenPGP Signing
│   └── xep-0384-ox-end-to-end.xml     # OpenPGP for XMPP
└── build/                    # Build output directories (debug/asan)
```

## Spec Files Legend

| Prefix | Type | Description |
|--------|------|-------------|
| rfcXXXX-\*.txt | RFC | IETF standards track documents |
| xep-XXXX-\*.xml | XEP | XMPP Extension Proposals |

### RFC Categories
- **Core XMPP**: rfc6120, rfc6121, rfc7622
- **Authentication**: rfc4616, rfc5802, rfc7677
- **Transport**: rfc8829, rfc8155, rfc8553
- **Relay**: rfc5766, rfc8656
- **Security**: rfc9266

### XEP Categories
- **Core**: xep-0030 (caps), xep-0166 (disco-info)
- **Messaging**: xep-0184 (receipts), xep-0280 (carbons), xep-0297 (hints)
- **Presence**: xep-0160 (presence-xml), xep-0249 (mini-presence)
- **Roster**: xep-0059 (roster versioning)
- **MUC**: xep-0045, xep-0245
- **Storage**: xep-0048 (bookmarks), xep-0049 (private)
- **Encryption**: xep-0373, xep-0374, xep-0384 (OX/OMEMO)
- **Advanced**: xep-0060 (pubsub), xep-0163 (pep), xep-0313 (MAM)