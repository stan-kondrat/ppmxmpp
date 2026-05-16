#!/usr/bin/env bash
#
# profanity-specific helpers.  Source this file in addition to _common.sh.
# These helpers depend on profanity-specific output formats and are not
# used by xmppc or xmpp-message tests.
#
# Usage:
#   source "$SCRIPT_DIR/_common.sh"
#   source "$SCRIPT_DIR/_helpers_profanity.sh"
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

# =====================================================================
# Screen-based profanity session helpers
# =====================================================================
#
# Pattern: profanity runs inside a named screen session so that we can
# inject commands via `screen -S <name> -X stuff` without launching a
# second profanity process (which would fight over the same account).
#
# Usage:
#   profanity_start SESSION_NAME CONFIG_HOME DATA_HOME ACCOUNT LOG_FILE
#   wait_for_session_registered "$JID" "$LABEL" "$LOG_FILE"
#   profanity_send   SESSION_NAME "/msg bob@localhost hello"
#   profanity_stop   SESSION_NAME PID
#

# profanity_start SESSION CONFIG_HOME DATA_HOME ACCOUNT LOG
# Launch profanity inside a detached screen session with a PTY so that
# profanity stays alive and we can inject commands via screen stuff.
# Prints the profanity PID to stdout.
profanity_start() {
    local session="$1" config_home="$2" data_home="$3" account="$4" log_file="$5"

    # TERM=xterm: required, profanity exits immediately without a terminal type.
    # stdbuf: ensures log output is flushed immediately to the log file.
    TERM=xterm screen -dmS "$session" \
        env TERM=xterm XDG_CONFIG_HOME="$config_home" XDG_DATA_HOME="$data_home" \
        stdbuf -oL -eL profanity -a "$account" -l DEBUG -f "$log_file"

    # Give screen a moment to fork the process
    sleep 0.5
    local pid
    pid=$(pgrep -f "profanity -a $account" | head -1 || true)
    log_debug "profanity_start: session=$session pid=${pid:-unknown}"
    echo "${pid:-}"
}

# profanity_send SESSION COMMAND
# Inject a command into a running profanity screen session.
# Appends a newline so profanity executes it immediately.
profanity_send() {
    local session="$1" cmd="$2"
    log_debug "profanity_send [$session]: $cmd"
    screen -S "$session" -X stuff "$cmd
"
}

# profanity_stop SESSION PID
# Send /quit to profanity then kill the screen session.
profanity_stop() {
    local session="$1" pid="$2"
    screen -S "$session" -X stuff "/quit
" 2>/dev/null || true
    sleep 0.2
    screen -S "$session" -X quit 2>/dev/null || true
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    screen -wipe 2>/dev/null || true
}

# dump_profanity_log LABEL LOG_FILE
# Print the last 40 lines of a profanity log (SENT/RECV/ERR/INF lines only)
# to debug output.  Call this on failure for context.
dump_profanity_log() {
    local label="$1" log_file="$2"
    log_debug "=== $label (last 40 relevant lines) ==="
    log_debug "$(grep -aE "SENT|RECV|ERR|INF|DBG.*[Ll]og" "$log_file" 2>/dev/null \
                | tail -40 || true)"
}

# dump_screen_sessions
# List all screen sessions for debugging.
dump_screen_sessions() {
    log_debug "=== active screen sessions ==="
    log_debug "$(screen -ls 2>/dev/null || true)"
}
