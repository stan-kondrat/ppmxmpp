#!/usr/bin/env bash
#
# E2E test: Blocking (XEP-0186 / jabber:iq:privacy) — step 27
#
# Feature: Blocking
# Dependency: Only needs IQ router + a new blocklist table
#
# Scenarios to cover:
#
#   Block
#     - Alice sends <iq type='set'><query xmlns='jabber:iq:privacy'><block><item jid='bob@localhost'/></block></query></iq>
#       → server returns <iq type='result'/>
#     - Block multiple JIDs in a single <block> stanza → all are stored, result returned
#     - <block/> with no <item> children → <bad-request> error
#     - Blocking the same JID twice → idempotent, no error
#
#   Unblock
#     - Alice unblocks bob → <iq type='result'/>, bob's JID removed from blocklist
#     - <unblock/> with no <item> children → <bad-request> error
#
#   Blocklist retrieval
#     - GET blocklist when empty → <iq type='result'> with empty <query xmlns='jabber:iq:privacy'>
#     - GET blocklist after blocking two JIDs → both JIDs appear in the response
#
#   Message delivery enforcement
#     - Bob (blocked by Alice) sends a <message> to Alice → message is silently dropped, not delivered
#     - Charlie (not blocked by Alice) sends a <message> to Alice → delivered normally
#     - After Alice unblocks Bob, Bob's next message is delivered
#
#   Presence enforcement (if implemented)
#     - Blocked contact sends <presence> to Alice → silently dropped
#
#   Service discovery
#     - disco#info includes the blocking feature namespace
#
# Usage:  ./test_e2e/blocking.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - two XMPP client sessions (or raw TCP feeds) on PATH
#   - ppmxmpp built (make)
#
