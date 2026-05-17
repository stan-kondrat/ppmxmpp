#!/usr/bin/env bash
#
# E2E test: Chat Markers (XEP-0333) — step 30
#
# Feature: Chat Markers
# Dependency: Thin message handler, no new storage
#
# Scenarios to cover:
#
#   Markable message routing
#     - Alice sends <message><body>hi</body><markable xmlns='urn:xmpp:chat-markers:0'/></message> to Bob
#       → Bob receives the message with <markable/> child intact
#     - The server does not strip, modify, or act on the <markable/> element
#
#   Displayed marker routing
#     - Bob sends <message to='alice@localhost'><displayed xmlns='urn:xmpp:chat-markers:0' id='msg-id'/></message>
#       → Alice receives the message with the <displayed id='msg-id'/> child intact
#     - The id attribute is preserved verbatim
#
#   Legacy marker routing (backward compatibility)
#     - Bob sends a <received xmlns='urn:xmpp:chat-markers:0' id='msg-id'/> marker message
#       → Alice receives it unchanged (server routes it as a normal message)
#
#   No server-side storage or generation
#     - Server does not generate any marker on behalf of clients
#     - Server does not store markers
#
#   Service discovery
#     - disco#info to server JID includes <feature var='urn:xmpp:chat-markers:0'/>
#
# Usage:  ./test_e2e/chat_markers.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - two XMPP client sessions on PATH
#   - ppmxmpp built (make)
#
