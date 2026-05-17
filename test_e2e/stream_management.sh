#!/usr/bin/env bash
#
# E2E test: Stream Management (XEP-0198) — step 15
#
# Feature: Stream Management
# Dependency: Pure connection-layer feature, no storage deps
#
# Scenarios to cover:
#
#   Feature advertisement
#     - After TLS + auth, <stream:features> includes <sm xmlns='urn:xmpp:sm:3'/>
#
#   Enabling SM
#     - <enable xmlns='urn:xmpp:sm:3'/> after bind → server responds with <enabled xmlns='urn:xmpp:sm:3'/>
#     - <enable resume='true'/> → response contains resume='true' and a non-empty id attribute
#     - Duplicate <enable/> on an already-enabled session → stream error (conflict or policy-violation)
#     - <enable/> before bind → silently ignored or <failed/>
#
#   Ack request and response
#     - Client sends <r/> after enable → server responds with <a xmlns='urn:xmpp:sm:3' h='N'/>
#     - h value in <a/> matches the number of stanzas server has received since <enable/>
#     - Client sends <r/> before enabling SM → ignored, no <a/> sent
#
#   Ack response from client
#     - Client sends <a h='N'/> where N equals stanzas sent by server → accepted silently
#     - Client sends <a h='N'/> where N > stanzas server has sent → <handled-count-too-high> stream error
#
#   Resumption
#     - <resume previd='unknown-id' h='0'/> → server responds with <failed><item-not-found/></failed>
#     - Client disconnects after <enable resume='true'/>, reconnects, sends <resume previd='...' h='0'/> →
#       server responds with <resumed h='...' previd='...'/>
#
# Usage:  ./test_e2e/stream_management.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - an XMPP client that can send raw SM stanzas on PATH
#   - ppmxmpp built (make)
#
