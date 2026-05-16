#!/usr/bin/env bash
#
# xmppc-specific helpers.  Source this file in addition to _common.sh.
# These helpers depend on xmppc-specific output formats and are not
# used by profanity or xmpp-message tests.
#
# Usage:
#   source "$SCRIPT_DIR/_common.sh"
#   source "$SCRIPT_DIR/_helpers_xmppc.sh"
#

# extract_full_jid CLIENT_LOG BARE_JID
# Extract the full JID from the <jid> element inside the RFC 6120 §7 bind
# IQ result stanza in the xmppc verbose log.  Falls back to BARE_JID if
# extraction fails.
# Example matched line:  <jid>bob@localhost/a1b2c3d4</jid>
extract_full_jid() {
    local log="$1"; local bare="$2"
    local full
    full=$(grep -oE "<jid>${bare}/[^<>]+</jid>" "$log" 2>/dev/null | \
           head -1 | sed 's/<jid>//;s/<\/jid>//' || true)
    if [ -z "$full" ]; then
        full="$bare"
    fi
    echo "$full"
}