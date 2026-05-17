#!/usr/bin/env bash
#
# E2E test: Roster Versioning (XEP-0237) — step 28
#
# Feature: Roster Versioning
# Dependency: Additive to existing roster (step 7, done)
#
# Scenarios to cover:
#
#   Feature advertisement
#     - disco#info or <stream:features> after auth includes <ver xmlns='urn:xmpp:features:rosterver'/>
#
#   Roster get with no cached version
#     - Client sends <iq type='get'><query xmlns='jabber:iq:roster'/></iq> (no ver attribute)
#       → server returns full roster with a ver= attribute on <query>
#
#   Roster get with matching version
#     - Client sends <iq type='get'><query xmlns='jabber:iq:roster' ver='<current-ver>'/></iq>
#       → server returns <iq type='result'> with empty body (no <query>) — client's roster is up to date
#
#   Roster get with stale version
#     - Client sends <iq type='get'><query xmlns='jabber:iq:roster' ver='<old-ver>'/></iq>
#       → server returns full roster (or delta pushes) with updated ver=
#
#   Roster push includes ver
#     - When a contact is added, the roster push <iq type='set'> includes ver= on <query>
#     - When a contact is removed (subscription='remove'), the roster push includes updated ver=
#     - Ver changes after each mutating operation (add, update, remove)
#
#   Ver stability
#     - Same roster state always produces the same ver value (deterministic hash)
#     - Adding then immediately removing a contact returns ver to a different value than the original
#       (content-hash, not sequence number)
#
# Usage:  ./test_e2e/roster_versioning.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - an XMPP client that sends ver= in roster get on PATH
#   - ppmxmpp built (make)
#
