#!/usr/bin/env bash
#
# E2E test: SCRAM-SHA-256 authentication — step 17
#
# Feature: SCRAM-SHA-256
# Dependency: Only touches auth layer (step 4, done) + users table migration
#
# Scenarios to cover:
#
#   Mechanism advertisement
#     - After TLS, <stream:features><mechanisms> lists SCRAM-SHA-256
#     - PLAIN is still listed alongside SCRAM-SHA-256
#     - Before TLS (plain TCP), SCRAM-SHA-256 is NOT advertised
#
#   Successful authentication (3-message exchange)
#     - Client sends <auth mechanism='SCRAM-SHA-256'> with base64(client-first-message)
#       → server replies <challenge> with base64(server-first-message) containing r=, s=, i=
#     - Server nonce in challenge starts with the client nonce verbatim
#     - Client sends <response> with base64(client-final-message including ClientProof)
#       → server replies <success> with base64(v=ServerSignature)
#     - Client can perform resource bind immediately after <success>
#
#   Authentication failures
#     - Correct username, wrong password → <failure><not-authorized/></failure>
#     - Unknown username → server still sends a synthetic <challenge> (timing safe),
#       then <failure><not-authorized/></failure> on client-final
#     - Malformed base64 in <auth> body → <failure><incorrect-encoding/></failure>
#     - Nonce in client-final does not start with the server-combined nonce
#       → <failure><not-authorized/></failure>
#
#   Session after auth
#     - After SCRAM-SHA-256 success, messaging works end-to-end between two authenticated sessions
#     - SCRAM-SHA-256 session behaves identically to PLAIN session post-auth
#       (roster fetch, message routing, etc.)
#
# Usage:  ./test_e2e/sasl_scram_sha256.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - an XMPP client supporting SCRAM-SHA-256 on PATH
#   - ppmxmpp built (make)
#
