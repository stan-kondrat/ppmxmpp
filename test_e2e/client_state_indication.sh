#!/usr/bin/env bash
#
# E2E test: Client State Indication (XEP-0352) — step 31
#
# Feature: Client State Indication
# Dependency: Connection-state flag only
#
# Scenarios to cover:
#
#   Feature advertisement
#     - After auth + bind, <stream:features> includes <csi xmlns='urn:xmpp:csi:0'/>
#
#   State transitions
#     - Client sends <inactive xmlns='urn:xmpp:csi:0'/> → server accepts silently (no reply stanza)
#     - Client sends <active xmlns='urn:xmpp:csi:0'/> after inactive → server accepts silently (no reply)
#     - Repeated <inactive/> is idempotent — no error, no reply
#     - Repeated <active/> is idempotent — no error, no reply
#
#   No reply rule
#     - Server MUST NOT send any stanza in direct response to <active/> or <inactive/>
#
#   Wrong namespace / element ignored
#     - <active xmlns='urn:xmpp:wrong'/> → silently ignored, session continues normally
#     - <foobar xmlns='urn:xmpp:csi:0'/> → silently ignored
#
#   Session continues normally after state change
#     - While inactive: messages sent to the client are still delivered (server does not buffer by default)
#     - After transitioning back to active: messaging continues without interruption
#
# Usage:  ./test_e2e/client_state_indication.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - an XMPP client capable of sending raw top-level stanzas on PATH
#   - ppmxmpp built (make)
#
