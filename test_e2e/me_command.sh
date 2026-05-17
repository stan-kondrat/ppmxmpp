#!/usr/bin/env bash
#
# E2E test: /me command (XEP-0245) — step 29
#
# Feature: /me command
# Dependency: Trivial message transform, no infrastructure
#
# Scenarios to cover:
#
#   Message routing — body unchanged
#     - Alice sends <message><body>/me waves</body></message> to Bob
#       → Bob receives the message with body exactly "/me waves" (no prefix stripped, no transformation)
#     - The body "/me waves" is NOT transformed to "* Alice waves" by the server
#     - A regular message (body not starting with /me) also passes through unchanged
#
#   Multi-resource delivery
#     - Bob is connected on two resources; /me message from Alice is delivered to both
#       with body intact on each resource
#
#   Offline delivery
#     - Bob is offline; Alice sends a /me message → message is stored in offline storage
#       with the original body unchanged and delivered when Bob reconnects
#
#   Service discovery
#     - disco#info to server JID includes <feature var='urn:xmpp:me-command:0'/>
#
# Usage:  ./test_e2e/me_command.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - two XMPP client sessions on PATH
#   - ppmxmpp built (make)
#
