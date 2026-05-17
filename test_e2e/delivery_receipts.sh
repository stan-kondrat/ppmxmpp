#!/usr/bin/env bash
#
# E2E test: Message Delivery Receipts (XEP-0184) — step 26
#
# Feature: Delivery Receipts
# Dependency: Simple message-layer feature, no new storage
#
# Scenarios to cover:
#
#   Receipt generation
#     - Alice sends a <message id='m1'> containing <request xmlns='urn:xmpp:receipts'/>
#       to Bob → server sends <message><received xmlns='urn:xmpp:receipts' id='m1'/></message>
#       back to Alice
#     - Receipt is sent to Alice's full JID (the sending resource), not bare JID
#     - Message without an id attribute but with <request/> → no receipt sent
#     - Message with <request/> but type='groupchat' → receipt behaviour per spec
#       (server does not generate; client-to-client only)
#
#   Receipt message routing
#     - Bob sends Alice a <message> containing <received id='m1'/> (incoming ack) →
#       that ack message is delivered to Alice normally
#     - A receipt message (<received/>) is excluded from Message Carbons
#       (not carbon-copied to Alice's other resources)
#
#   Service discovery
#     - disco#info to server JID includes <feature var='urn:xmpp:receipts'/>
#
#   No-receipt cases
#     - Message with no <request/> child → no <received/> sent back, message forwarded normally
#     - Message to an offline user with <request/> → receipt is NOT sent
#       (server cannot confirm delivery to an offline client)
#
# Usage:  ./test_e2e/delivery_receipts.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - an XMPP client supporting XEP-0184 on PATH (or raw TCP feed)
#   - ppmxmpp built (make)
#
