#!/usr/bin/env bash
#
# xmpp-message-specific helpers.  Source this file in addition to _common.sh.
# These helpers depend on xmpp-message output formats and are not used by
# xmppc or profanity tests.
#
# Usage:
#   source "$SCRIPT_DIR/_common.sh"
#   source "$SCRIPT_DIR/_helpers_xmpp-message.sh"
#

# assert_log FILE LABEL PATTERN
# Assert PATTERN exists in FILE and pass/fail accordingly.
assert_log() {
    local file="$1"; local label="$2"; local pattern="$3"
    if grep_log "$file" "$pattern"; then
        pass "$label"
    else
        log_debug "=== $label failure details ==="
        log_debug "Pattern: $pattern"
        log_debug "=== First 30 lines of $file ==="
        log_debug "$(head -30 "$file" 2>/dev/null)"
        fail "$label — pattern not found: $pattern"
    fi
}

# assert_not_log FILE LABEL PATTERN
# Assert PATTERN does NOT exist in FILE and pass/fail accordingly.
assert_not_log() {
    local file="$1"; local label="$2"; local pattern="$3"
    if grep_log "$file" "$pattern"; then
        log_debug "=== $label failure: pattern SHOULD NOT be found ==="
        log_debug "Pattern: $pattern"
        fail "$label — pattern should not exist: $pattern"
    else
        pass "$label (pattern '$pattern' not found — OK)"
    fi
}

# wait_for_file FILE TIMEOUT_SEC
# Wait for FILE to exist, up to TIMEOUT_SEC.
# Returns 0 on file found, 1 on timeout.
wait_for_file() {
    local file="$1"; local timeout="$2"
    local elapsed=0
    while [ $elapsed -lt "$timeout" ]; do
        [ -f "$file" ] && return 0
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    return 1
}