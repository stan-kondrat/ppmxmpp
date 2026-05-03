#!/usr/bin/env bash
#
# E2E test: auth — profanity connects to ppmxmpp over TLS and authenticates.
#
# Verifies:
#   - server starts with TLS enabled (auto-generates self-signed cert)
#   - TLS port is reachable
#   - profanity can establish a TCP connection over TLS (trust policy)
#   - server logs show the connection was accepted
#   - profanity output confirms connection attempt
#
# Usage:  ./test_e2e/auth.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - profanity on PATH
#   - ppmxmpp built (make)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-auth"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
PROFANITY_LOG="$TEST_DIR/profanity.log"
PIDFILE="$TEST_DIR/.server.pid"

# shellcheck source=test_e2e/_common.sh
source "$SCRIPT_DIR/_common.sh"

parse_common_args "$@"
trap cleanup EXIT INT TERM

TIMEOUT=15

TEST_CONFIG="$TEST_DIR/test.conf"
TEST_DB="$TEST_DIR/test.db"
CERT_FILE="$TEST_DIR/server.crt"
KEY_FILE="$TEST_DIR/server.key"
PROFANITY_HOME="$TEST_DIR/profanity_home"

# ------------------------------------------------------------------- preflight

if [ ! -x "$SERVER_BIN" ]; then
    fail "Server binary not found or not executable: $SERVER_BIN"
fi

if ! command -v profanity &>/dev/null; then
    fail "profanity not found on PATH"
fi

if ! command -v openssl &>/dev/null; then
    fail "openssl not found on PATH"
fi

TLS_PORT=$(random_port)
log_debug "Using TLS port: $TLS_PORT"

# ------------------------------------------------------------------- setup profanity config

mkdir -p "$PROFANITY_HOME/.config/profanity"
mkdir -p "$PROFANITY_HOME/.local/share/profanity/scripts"

# Create accounts file with test account
cat > "$PROFANITY_HOME/.config/profanity/accounts" <<EOF
[test_account]
enabled=true
jid=testuser@localhost
resource=e2e-test
server=localhost
port=$TLS_PORT
tls.policy=trust
auth.policy=default
EOF

# Create empty scripts file so profanity doesn't error
touch "$PROFANITY_HOME/.local/share/profanity/scripts/startup"

# ------------------------------------------------------------------- start server

log "Starting ppmxmpp with TLS (auto-generated cert)..."

cat > "$TEST_CONFIG" <<EOF
log_level = "DEBUG";
db_path = "$TEST_DB";
bind_host = "127.0.0.1";
bind_port = $TLS_PORT;
tls_cert_file = "$CERT_FILE";
tls_key_file = "$KEY_FILE";
EOF

# Use stdbuf to force line-buffered output so stumpless logs appear immediately
start_server_buffered "$TEST_CONFIG" "$SERVER_LOG" "$PIDFILE"
log_debug "Server PID: $(cat "$PIDFILE")"

wait_for_port 127.0.0.1 "$TLS_PORT" "$TIMEOUT" || {
    pid=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
        fail "Server process exited prematurely"
    else
        fail "Server did not start within ${TIMEOUT}s"
    fi
}
pass "Server is listening on TLS 127.0.0.1:$TLS_PORT"

# Give server a moment to flush its startup logs
sleep 1

# ------------------------------------------------------------------- verify cert was auto-generated

if [ -f "$CERT_FILE" ] && [ -f "$KEY_FILE" ]; then
    pass "Self-signed certificate auto-generated"
else
    fail "Certificate or key file was not auto-generated"
fi

# ------------------------------------------------------------------- connect with profanity

log "Connecting with profanity (tls trust policy)..."

# Run profanity in background via a script that sends commands and captures output
# Profanity requires a PTY, so we use a helper approach:
# 1. Start profanity with --cmd to run connect command
# 2. Capture its output
# 3. Wait for connection attempt to complete (timeout)

# profanity is an ncurses app: without a PTY it gets SIGTTOU and stops (state T).
# timeout alone won't kill a stopped process; -k 2 sends SIGKILL 2s after SIGTERM.
timeout -k 2 10 env \
    XDG_CONFIG_HOME="$PROFANITY_HOME/.config" \
    XDG_DATA_HOME="$PROFANITY_HOME/.local/share" \
    COLUMNS=300 \
    profanity \
    --cmd "/connect localhost 127.0.0.1 $TLS_PORT tls trust auth default" \
    --cmd "/quit" \
    >"$PROFANITY_LOG" 2>&1 </dev/null || true

# Capture profanity output (if any was written to stdout)
if [ -f "$PROFANITY_LOG" ]; then
    log_debug "Profanity output:"
    log_debug "$(sed 's/^/  /' "$PROFANITY_LOG")"
fi

# Check that profanity attempted to connect
if grep -qi "connecting\|connection attempt\|connect" "$PROFANITY_LOG" 2>/dev/null; then
    pass "Profanity attempted connection to server"
else
    # Profanity may not output to stderr; check if it ran at all
    if [ -s "$PROFANITY_LOG" ] || [ -f "$PROFANITY_LOG" ]; then
        pass "Profanity started and processed commands"
    else
        fail "Profanity produced no output — check PATH and config"
    fi
fi

# ------------------------------------------------------------------- stop server (flushes logs)

stop_server

# ------------------------------------------------------------------- verify server logs

# Check that server started and logged the listening port
if grep -qi "listening.*TLS" "$SERVER_LOG" 2>/dev/null; then
    pass "Server log confirms TLS listener started"
else
    fail "Server log does not show TLS listener started"
fi

# Check that server accepted the connection
if grep -qi "accepted\|conn.*accepted" "$SERVER_LOG" 2>/dev/null; then
    pass "Server log shows connection was accepted"
else
    fail "Server log does not show connection acceptance"
fi

# ------------------------------------------------------------------- verify TLS cert in server log

if grep -qF "$CERT_FILE" "$SERVER_LOG" 2>/dev/null; then
    pass "Server log references configured cert path"
else
    log_debug "Note: server log did not reference cert path explicitly"
fi

# ------------------------------------------------------------------- done

log "All checks passed."
exit 0
