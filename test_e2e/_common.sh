#!/usr/bin/env bash
# Shared helpers for e2e tests.  Source this file; do not execute directly.
#
# Tests must set before sourcing (or before calling any function):
#   TEST_NAME  - log prefix (e.g. "e2e-tls")
#   SERVER_BIN - absolute path to the ppmxmpp binary
#   SERVER_LOG - path to server log file
#   CLIENT_LOG - path to client log file (may be unset)
#   PIDFILE    - path to server PID file
#   TEST_DIR   - temp directory removed on cleanup
#   DEBUG      - "true" to preserve temp dir and show debug output

log() {
    echo "[$TEST_NAME] $*"
}

log_debug() {
    if [ "$DEBUG" = true ]; then
        echo "[debug] $*"
    fi
}

fail() {
    log "FAIL: $*"
    echo ""
    echo "=== Server log ==="
    cat "$SERVER_LOG" 2>/dev/null || echo "(no server log)"
    if [ -n "${CLIENT_LOG:-}" ]; then
        echo ""
        echo "=== Client log ==="
        cat "$CLIENT_LOG" 2>/dev/null || echo "(no client log)"
    fi
    exit 1
}

pass() {
    log "PASS: $*"
}

cleanup() {
    local exit_code=$?
    if [ -f "$PIDFILE" ]; then
        local pid
        pid=$(cat "$PIDFILE" 2>/dev/null)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
        rm -f "$PIDFILE"
    fi
    # Kill any screen sessions started by this test run (named ppmxmpp_*_$$).
    local screens
    screens=$(screen -ls 2>/dev/null | grep -oE "[0-9]+\.ppmxmpp_[^[:space:]]+_$$" 2>/dev/null || true)
    if [ -n "$screens" ]; then
        while IFS= read -r s; do screen -S "$s" -X quit 2>/dev/null || true; done <<< "$screens"
    fi
    screen -wipe 2>/dev/null 1>/dev/null || true
    if [ "$DEBUG" = true ]; then
        log "Debug mode: temp dir preserved at $TEST_DIR"
        log "Press Enter to remove temp dir and exit..."
        read -r
    fi
    rm -rf "$TEST_DIR"
    exit "$exit_code"
}

# start_server CONFIG LOG PIDFILE
# Starts the server in background. Writes PID to PIDFILE.
start_server() {
    local config="$1" log_file="$2" pid_file="$3"
    touch "$log_file"
    "$SERVER_BIN" --config "$config" >> "$log_file" 2>&1 &
    echo $! > "$pid_file"
}

# start_server_buffered CONFIG LOG PIDFILE
# Same as start_server but uses stdbuf to force line-buffered stdout
# so stumpless logs appear immediately in the log file.
start_server_buffered() {
    local config="$1" log_file="$2" pid_file="$3"
    touch "$log_file"
    stdbuf -oL -eL "$SERVER_BIN" --config "$config" >> "$log_file" 2>&1 &
    echo $! > "$pid_file"
}

# stop_server
# Sends SIGTERM to the server and waits for it to exit, flushing its logs.
# Safe to call even if the server is already dead.
stop_server() {
    local pid
    pid=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -f "$PIDFILE"
}

# random_port
# Prints a free ephemeral port by asking the kernel for one via a temporary
# server socket, then releasing it. Pure bash + nc; no python3 required.
random_port() {
    local port
    # ss prints "State Recv-Q Send-Q Local..." — extract port numbers in use.
    local used
    used=$(ss -tlnH 2>/dev/null | awk '{print $4}' | grep -oE '[0-9]+$' | sort -n)
    while true; do
        port=$(( (RANDOM << 1 | RANDOM & 1) % 16384 + 49152 ))
        if ! echo "$used" | grep -qx "$port"; then
            echo "$port"
            return
        fi
    done
}

# wait_for_port HOST PORT TIMEOUT
# Returns 0 when the TCP port accepts connections.
# Returns non-zero on timeout or if the server process dies.
# Distinguishes server death via exit code 2; timeout via exit code 1.
wait_for_port() {
    local host="$1" port="$2" timeout_sec="${3:-15}"
    local elapsed=0
    while [ "$elapsed" -lt "$timeout_sec" ]; do
        if ! kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
            return 2
        fi
        if bash -c "echo >/dev/tcp/$host/$port" 2>/dev/null; then
            return 0
        fi
        log_debug "Waiting for $host:$port... (${elapsed}s)"
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    return 1
}

# =====================================================================
# Pattern matching helpers
# =====================================================================

# grep_log FILE PATTERN
# Returns 0 (success) if PATTERN is found in FILE, 1 otherwise.
grep_log() {
    local file="$1"; local pattern="$2"
    grep -qaE "$pattern" "$file" 2>/dev/null
}

# wait_for_pattern FILE PATTERN TIMEOUT_SEC
# Polls FILE every 0.5s until PATTERN appears or TIMEOUT is reached.
# Returns 0 on match, 1 on timeout.
wait_for_pattern() {
    local file="$1"; local pattern="$2"; local timeout_sec="$3"
    local elapsed=0
    while [ "$elapsed" -lt "$timeout_sec" ]; do
        if grep_log "$file" "$pattern"; then
            return 0
        fi
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    return 1
}

# =====================================================================
# Session table helpers
# =====================================================================

# wait_for_session_registered JID CLIENT_NAME CLIENT_LOG
# Waits up to 20 seconds for a client to complete its connection lifecycle
# (TCP + TLS + SASL + bind + initial presence) and be registered in the
# server session table.  Polls the server log for the registration entry.
# Works with any XMPP client (xmppc, profanity, xmpp-message, etc.) because
# it only inspects the server-side log.  Exits via fail() on timeout.
wait_for_session_registered() {
    local jid="$1"; local name="$2"; local client_log="$3"
    if wait_for_pattern "$SERVER_LOG" "session: registered ${jid}" 20; then
        pass "$name is online and registered in session table"
    else
        log_debug "=== $name log ==="
        log_debug "$(cat "$client_log")"
        fail "$name did not register within 20s — check server and client logs"
    fi
}

# =====================================================================
# Common argument parsing
# =====================================================================

# parse_common_args [args...]
# Sets DEBUG=true if --debug is present.
parse_common_args() {
    DEBUG=false
    for arg in "$@"; do
        case "$arg" in
            --debug) DEBUG=true ;;
        esac
    done
}
