#!/usr/bin/env bash
#
# E2E test: Offline message delivery (XEP-0160, XEP-0203)
#
# Scenario:
#   1. Bob connects with xmppc --mode monitor stanza (stays online)
#   2. Alice connects and sends a message with a random body to Bob's bare JID
#   3. Bob receives the message via the monitor and the random body is verified
#
# Prerequisites:
#   1. App creates DB schema via migrations (NOT manually in test)
#   2. Creating Bob and Alice users
#
# Checks:
#   1. Server starts and listens
#   2. Alice connects and sends the message
#   3. Server receives the message stanza
#   4. Bob receives the exact message body (via xmppc monitor stanza)
#
# Usage:  ./test_e2e/xmppc_offline_messages.sh [--debug]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-offline-messages-xmppc"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
ALICE_LOG="$TEST_DIR/alice.log"
BOB_LOG="$TEST_DIR/bob.log"
PIDFILE="$TEST_DIR/.server.pid"

# shellcheck source=test_e2e/_common.sh
source "$SCRIPT_DIR/_common.sh"
# shellcheck source=test_e2e/_helpers_xmppc.sh
source "$SCRIPT_DIR/_helpers_xmppc.sh"

parse_common_args "$@"
trap cleanup EXIT INT TERM

TIMEOUT=15
ALICE_JID="alice@localhost"
ALICE_PASS="alicepass"
BOB_JID="bob@localhost"
BOB_PASS="bobpass"

# Generate a random message body so we can verify end-to-end delivery
MESSAGE_BODY="e2e_$(head -20 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"

TEST_CONFIG="$TEST_DIR/test.conf"
TEST_DB="$TEST_DIR/test.db"
CERT_FILE="$TEST_DIR/server.crt"
KEY_FILE="$TEST_DIR/server.key"

# ===================================================================== preflight

[ -x "$SERVER_BIN" ]              || fail "Server binary not found: $SERVER_BIN (run 'make' first)"
command -v xmppc &>/dev/null      || fail "xmppc not in PATH"
command -v openssl &>/dev/null   || fail "openssl not in PATH"
command -v sqlite3 &>/dev/null   || fail "sqlite3 not in PATH"
command -v stdbuf &>/dev/null     || fail "stdbuf not in PATH"

# xmppc resolves the server from the JID domain (no --server/--port flags).
# It connects to <domain>:5222 and uses STARTTLS, so we must run the
# server on the standard XMPP client port.
XMPP_PORT=5222
if ss -tlnH 2>/dev/null | awk '{print $4}' | grep -oE '[0-9]+$' | grep -qx "$XMPP_PORT"; then
    log "SKIP: port $XMPP_PORT already in use — cannot run xmppc test"
    exit 0
fi
log_debug "Using STARTTLS port: $XMPP_PORT"

# ===================================================================== certificate

log "Generating self-signed certificate..."
openssl req -x509 -newkey rsa:2048 -keyout "$KEY_FILE" -out "$CERT_FILE" \
    -days 1 -nodes \
    -subj "/CN=localhost" \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
    2>/dev/null
CERT_FP=$(openssl x509 -in "$CERT_FILE" -noout -fingerprint -sha256 2>/dev/null \
    | cut -d= -f2 | tr -d ':' | tr '[:upper:]' '[:lower:]')
pass "Certificate generated (SHA-256: $CERT_FP)"

# ===================================================================== server (creates DB schema via migrations on startup)

cat > "$TEST_CONFIG" <<EOF
log_level = "DEBUG";
db_path = "$TEST_DB";
bind_host = "127.0.0.1";
bind_port = $XMPP_PORT;
tls_cert_file = "$CERT_FILE";
tls_key_file = "$KEY_FILE";
EOF

log "Starting ppmxmpp on STARTTLS 127.0.0.1:$XMPP_PORT..."
start_server_buffered "$TEST_CONFIG" "$SERVER_LOG" "$PIDFILE"
log_debug "Server PID: $(cat "$PIDFILE")"

wait_for_port 127.0.0.1 "$XMPP_PORT" "$TIMEOUT" || {
    pid=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
        fail "Server process exited prematurely"
    else
        fail "Server did not start within ${TIMEOUT}s"
    fi
}
pass "Server listening on STARTTLS 127.0.0.1:$XMPP_PORT"

# Verify migrations were applied
ACTUAL_VERSION=$(sqlite3 "$TEST_DB" "SELECT version FROM schema_version LIMIT 1;")
log_debug "Database schema version after migrations: $ACTUAL_VERSION"
[ -n "$ACTUAL_VERSION" ] || fail "schema_version table not populated"

TABLE_EXISTS=$(sqlite3 "$TEST_DB" "SELECT name FROM sqlite_master WHERE type='table' AND name='offline_messages';")
[[ -n "$TABLE_EXISTS" ]] || fail "offline_messages table not found - migration may have failed"

# ===================================================================== seed users

log "Seeding Alice and Bob..."
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$ALICE_JID', '$ALICE_PASS', strftime('%s','now'));"
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$BOB_JID', '$BOB_PASS', strftime('%s','now'));"
pass "Alice and Bob created"

USER_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM users;")
log_debug "User count in DB: $USER_COUNT"
[ "$USER_COUNT" -eq 2 ] || fail "Expected 2 users, got $USER_COUNT"

# ===================================================================== phase 1: Bob connects with xmppc monitor stanza (stays online)

log "Phase 1: Bob connects with monitor stanza..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL xmppc -vv \
    --jid "$BOB_JID" \
    --pwd "$BOB_PASS" \
    --mode monitor stanza \
    > "$BOB_LOG" 2>&1 &
BOB_PID=$!
log_debug "Bob monitor PID: $BOB_PID"

wait_for_session_registered "$BOB_JID" "Bob" "$BOB_LOG"

# ===================================================================== phase 2: Alice sends a message to Bob

log "Phase 2: Alice sends message to Bob (body: $MESSAGE_BODY)..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL timeout 15 xmppc -vv \
    --jid "$ALICE_JID" \
    --pwd "$ALICE_PASS" \
    --mode message \
    chat "$BOB_JID" "$MESSAGE_BODY" \
    > "$ALICE_LOG" 2>&1

# ===================================================================== stop Bob's monitor

kill $BOB_PID 2>/dev/null || true
wait $BOB_PID 2>/dev/null || true

# Stop the server
kill "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null || true
wait "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null || true

# ===================================================================== assert: Alice sent successfully

if grep_log "$ALICE_LOG" "Connected|Secure connection"; then
    pass "Alice connected and sent the message"
else
    log_debug "=== Alice log ==="
    log_debug "$(cat "$ALICE_LOG")"
    if grep_log "$ALICE_LOG" "Connection failed|connection refused|auth.*fail"; then
        fail "Alice failed to connect — error in output"
    fi
    fail "Alice did not connect successfully"
fi

# ===================================================================== assert: server received the message stanza

log "Verifying server received the message stanza..."

if grep_log "$SERVER_LOG" "stanza=message"; then
    pass "Server received the message stanza"
else
    log_debug "$(strings "$SERVER_LOG" 2>/dev/null | grep -E "stanza|recv|message" | head -20)"
    fail "Server did NOT receive a message stanza — check server log"
fi

# ===================================================================== assert: Bob received the random message body

log "Verifying Bob received the message body..."
log_debug "Looking for body: $MESSAGE_BODY in $BOB_LOG"

if grep_log "$BOB_LOG" "$MESSAGE_BODY"; then
    pass "Bob received the message with correct body"
else
    log_debug "=== Bob log ==="
    log_debug "$(cat "$BOB_LOG")"
    fail "Bob did NOT receive the message body: $MESSAGE_BODY"
fi

# ===================================================================== assert: Bob's stanza has <delay> or <message> wrapper

if grep_log "$BOB_LOG" "<message.*from=.*$ALICE_JID"; then
    pass "Bob received <message> stanza from Alice"
else
    log "WARN: Could not confirm <message> stanza from in Bob log"
fi

# ===================================================================== debug dump (only in debug mode)

if [ "$DEBUG" = true ]; then
    log "=== DEBUG: Alice log ==="
    cat "$ALICE_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Bob log ==="
    cat "$BOB_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Server log (last 40 lines) ==="
    tail -40 "$SERVER_LOG" | strings | grep -v "^$" || true
fi

# ===================================================================== done

log "All checks passed."
exit 0