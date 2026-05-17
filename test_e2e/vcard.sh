#!/usr/bin/env bash
#
# E2E test: vCard (XEP-0054) — step 14
#
# Feature: vCard
# Dependency: Only needs IQ router (step 6, done) + a new SQLite table
#
# Scenarios to cover:
#
#   Own vCard — get/set lifecycle
#     - GET own vCard before any SET → server returns <iq type='result'> with empty <vCard xmlns='vcard-temp'/>
#     - SET own vCard with fields (FN, NICKNAME, EMAIL) → server returns <iq type='result'/>
#     - GET own vCard after SET → returns exactly the stored fields
#     - SET own vCard again with different content → GET returns only the latest version (replace, not merge)
#     - SET with empty <vCard/> body → clears stored data; subsequent GET returns empty <vCard xmlns='vcard-temp'/>
#
#   Own vCard — addressing variants
#     - SET with to='user@localhost' (own bare JID) → succeeds identically to no-to case
#     - GET with to='user@localhost' (own bare JID) → returns own vCard
#
#   Other users' vCards
#     - GET another user's vCard when they have no stored vCard → returns empty <vCard xmlns='vcard-temp'/>
#     - GET another user's vCard when they have a stored vCard → returns their vCard content
#     - GET vCard for a JID on a foreign domain → returns <item-not-found>
#     - GET vCard for a local JID that does not exist → returns <service-unavailable>
#
#   Authorization
#     - SET vCard to='other@localhost' (different user) → returns <forbidden>
#
#   Service discovery
#     - disco#info to server JID includes <feature var='vcard-temp'/>
#
# Usage:  ./test_e2e/vcard.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - an XMPP client capable of raw IQ sends (xmppc or equivalent) on PATH
#   - ppmxmpp built (make)
#
