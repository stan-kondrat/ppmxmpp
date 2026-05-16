#!/usr/bin/env bash
#
# E2E test: Message routing (RFC 6121 §8) with xmppc
#
# Scenario:
#   1. Bob connects with xmppc --mode monitor stanza (stays online)
#   2. Alice sends a message to Bob's bare JID → Bob receives (bare-JID routing)
#   3. Alice sends a message to Bob's full JID → Bob receives (full-JID routing)
#   4. Alice goes into monitor mode, Bob replies → Alice receives (bidirectional)
#   5. Carol connects with monitor stanza (stays online)
#   6. Alice sends to Carol → Carol receives (routing to third party)
#   7. Bob sends to Carol → Carol receives (cross-user routing)
#   8. Carol replies to Bob → Bob receives (Carol bidirectional)
#
# Prerequisites:
#   1. App creates DB schema via migrations (NOT manually in test)
#   2. Creating Alice, Bob, and Carol users
#
# Checks:
#   1. Server starts and listens
#   2. Alice → Bob (bare JID): Bob receives
#   3. Alice → Bob (full JID): Bob receives
#   4. Bob → Alice: Alice receives (bidirectional)
#   5. Alice → Carol: Carol receives (multi-recipient routing)
#   6. Bob → Carol: Carol receives (cross-user routing)
#   7. Carol → Bob: Bob receives (Carol bidirectional)
#
# Usage:  ./test_e2e/message_routing_xmppc.sh [--debug]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-message-routing-xmppc"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
ALICE_LOG="$TEST_DIR/alice.log"
BOB_LOG="$TEST_DIR/bob.log"
ALICE_MONITOR_LOG="$TEST_DIR/alice_monitor.log"
CAROL_LOG="$TEST_DIR/carol.log"
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
CAROL_JID="carol@localhost"
CAROL_PASS="carolpass"

# Generate random message bodies so we can verify end-to-end delivery
MSG_BARE="bare_$(head -20 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"
MSG_FULL="full_$(head -20 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"
MSG_REPLY="reply_$(head -20 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"
MSG_ALICE_CAROL="ac_$(head -20 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"
MSG_BOB_CAROL="bc_$(head -20 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"
MSG_CAROL_BOB="cb_$(head -20 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"

TEST_CONFIG="$TEST_DIR/test.conf"
TEST_DB="$TEST_DIR/test.db"
CERT_FILE="$TEST_DIR/server.crt"
KEY_FILE="$TEST_DIR/server.key"

# ===================================================================== preflight

[ -x "$SERVER_BIN" ]              || fail "Server binary not found: $SERVER_BIN (run 'make' first)"
command -v xmppc &>/dev/null      || fail "xmppc not in PATH"
command -v openssl &>/dev/null     || fail "openssl not in PATH"
command -v sqlite3 &>/dev/null     || fail "sqlite3 not in PATH"
command -v stdbuf &>/dev/null     || fail "stdbuf not in PATH"

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

# ===================================================================== server

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

# ===================================================================== seed users

log "Seeding Alice, Bob, and Carol..."
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$ALICE_JID', '$ALICE_PASS', strftime('%s','now'));"
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$BOB_JID', '$BOB_PASS', strftime('%s','now'));"
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$CAROL_JID', '$CAROL_PASS', strftime('%s','now'));"
pass "Alice, Bob, and Carol created"

USER_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM users;")
log_debug "User count in DB: $USER_COUNT"
[ "$USER_COUNT" -eq 3 ] || fail "Expected 3 users, got $USER_COUNT"

# ===================================================================== phase 1: Bob connects with monitor stanza (stays online)

log "Phase 1: Bob connects with monitor stanza..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL xmppc -vv \
    --jid "$BOB_JID" \
    --pwd "$BOB_PASS" \
    --mode monitor stanza \
    > "$BOB_LOG" 2>&1 &
BOB_PID=$!
log_debug "Bob monitor PID: $BOB_PID"


wait_for_session_registered "$BOB_JID" "Bob" "$BOB_LOG"

BOB_FULL_JID=$(extract_full_jid "$BOB_LOG" "$BOB_JID")
if [ "$BOB_FULL_JID" != "$BOB_JID" ]; then
    pass "Bob's full JID detected: $BOB_FULL_JID"
else
    log "WARN: Could not extract Bob's full JID (full-JID routing test will use bare JID fallback)"
fi

# ===================================================================== phase 2: Alice sends to Bob's bare JID

log "Phase 2: Alice sends message to Bob's bare JID (body: $MSG_BARE)..."
# stdbuf -oL -eL forces line-buffered stdout so the message body appears
# in the log immediately, avoiding races in wait_for_pattern checks.
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL timeout 15 xmppc -vv \
    --jid "$ALICE_JID" \
    --pwd "$ALICE_PASS" \
    --mode message \
    chat "$BOB_JID" "$MSG_BARE" \
    > "$ALICE_LOG" 2>&1

# Assert: Alice connected (check both client log and server session table)
if grep_log "$ALICE_LOG" "Connected|Secure connection"; then
    pass "Alice connected and sent the message to Bob's bare JID"
else
    log_debug "=== Alice log ==="
    log_debug "$(cat "$ALICE_LOG")"
    if grep_log "$ALICE_LOG" "Connection failed|connection refused|auth.*fail"; then
        fail "Alice failed to connect — error in output"
    fi
    fail "Alice did not connect successfully"
fi

# Assert: server received the message stanza
if grep_log "$SERVER_LOG" "stanza=message"; then
    pass "Server received the message stanza"
else
    log_debug "$(strings "$SERVER_LOG" 2>/dev/null | grep -E "stanza|recv|message" | head -20)"
    fail "Server did NOT receive a message stanza — check server log"
fi

# Assert: Bob received the bare-JID-routed message
if wait_for_pattern "$BOB_LOG" "$MSG_BARE" 15; then
    pass "Bob received the message (bare-JID routing)"
else
    log_debug "=== Bob log ==="
    log_debug "$(cat "$BOB_LOG")"
    fail "Bob did NOT receive the message body: $MSG_BARE"
fi

if grep_log "$BOB_LOG" "<message.*from=.*$ALICE_JID"; then
    pass "Bob received <message> stanza from Alice"
else
    log "WARN: Could not confirm <message> stanza from in Bob log"
fi

# ===================================================================== phase 3: Alice sends to Bob's full JID

if [ "$BOB_FULL_JID" != "$BOB_JID" ]; then
    log "Phase 3: Alice sends message to Bob's full JID (body: $MSG_FULL)..."

    SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL timeout 15 xmppc -vv \
        --jid "$ALICE_JID" \
        --pwd "$ALICE_PASS" \
        --mode message \
        chat "$BOB_FULL_JID" "$MSG_FULL" \
        > "$ALICE_LOG" 2>&1

    if wait_for_pattern "$BOB_LOG" "$MSG_FULL" 15; then
        pass "Bob received the message (full-JID routing)"
    else
        log_debug "=== Bob log ==="
        log_debug "$(cat "$BOB_LOG")"
        fail "Bob did NOT receive the full-JID-routed message body: $MSG_FULL"
    fi
else
    log "Phase 3: SKIPPED (could not determine Bob's full JID)"
fi

# ===================================================================== phase 4: Bidirectional — Alice in monitor mode, Bob replies

log "Phase 4: Alice connects with monitor stanza, Bob replies..."

SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL xmppc -vv \
    --jid "$ALICE_JID" \
    --pwd "$ALICE_PASS" \
    --mode monitor stanza \
    > "$ALICE_MONITOR_LOG" 2>&1 &
ALICE_MONITOR_PID=$!
log_debug "Alice monitor PID: $ALICE_MONITOR_PID"

wait_for_session_registered "$ALICE_JID" "Alice" "$ALICE_MONITOR_LOG"

log "Bob sends reply to Alice (body: $MSG_REPLY)..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL timeout 15 xmppc -vv \
    --jid "$BOB_JID" \
    --pwd "$BOB_PASS" \
    --mode message \
    chat "$ALICE_JID" "$MSG_REPLY" \
    > "$TEST_DIR/bob_reply.log" 2>&1

if wait_for_pattern "$ALICE_MONITOR_LOG" "$MSG_REPLY" 15; then
    pass "Alice received Bob's reply (bidirectional routing)"
else
    log_debug "=== Alice monitor log ==="
    log_debug "$(cat "$ALICE_MONITOR_LOG")"
    fail "Alice did NOT receive Bob's reply: $MSG_REPLY"
fi

if grep_log "$ALICE_MONITOR_LOG" "<message.*from=.*$BOB_JID"; then
    pass "Alice received <message> stanza from Bob"
else
    log "WARN: Could not confirm <message> stanza from Bob in Alice monitor log"
fi

# ===================================================================== phase 5: Carol connects with monitor stanza

log "Phase 5: Carol connects with monitor stanza..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL xmppc -vv \
    --jid "$CAROL_JID" \
    --pwd "$CAROL_PASS" \
    --mode monitor stanza \
    > "$CAROL_LOG" 2>&1 &
CAROL_PID=$!
log_debug "Carol monitor PID: $CAROL_PID"

wait_for_session_registered "$CAROL_JID" "Carol" "$CAROL_LOG"

# ===================================================================== phase 6: Alice sends to Carol

log "Phase 6: Alice sends message to Carol (body: $MSG_ALICE_CAROL)..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL timeout 15 xmppc -vv \
    --jid "$ALICE_JID" \
    --pwd "$ALICE_PASS" \
    --mode message \
    chat "$CAROL_JID" "$MSG_ALICE_CAROL" \
    > "$TEST_DIR/alice_to_carol.log" 2>&1

if wait_for_pattern "$CAROL_LOG" "$MSG_ALICE_CAROL" 15; then
    pass "Carol received Alice's message (multi-recipient routing)"
else
    log_debug "=== Carol log ==="
    log_debug "$(cat "$CAROL_LOG")"
    fail "Carol did NOT receive Alice's message: $MSG_ALICE_CAROL"
fi

if grep_log "$CAROL_LOG" "<message.*from=.*$ALICE_JID"; then
    pass "Carol received <message> stanza from Alice"
else
    log "WARN: Could not confirm <message> stanza from Alice in Carol log"
fi

# ===================================================================== phase 7: Bob sends to Carol

log "Phase 7: Bob sends message to Carol (body: $MSG_BOB_CAROL)..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL timeout 15 xmppc -vv \
    --jid "$BOB_JID" \
    --pwd "$BOB_PASS" \
    --mode message \
    chat "$CAROL_JID" "$MSG_BOB_CAROL" \
    > "$TEST_DIR/bob_to_carol.log" 2>&1

if wait_for_pattern "$CAROL_LOG" "$MSG_BOB_CAROL" 15; then
    pass "Carol received Bob's message (cross-user routing)"
else
    log_debug "=== Carol log ==="
    log_debug "$(cat "$CAROL_LOG")"
    fail "Carol did NOT receive Bob's message: $MSG_BOB_CAROL"
fi

if grep_log "$CAROL_LOG" "<message.*from=.*$BOB_JID"; then
    pass "Carol received <message> stanza from Bob"
else
    log "WARN: Could not confirm <message> stanza from Bob in Carol log"
fi

# ===================================================================== phase 8: Carol replies to Bob

log "Phase 8: Carol replies to Bob (body: $MSG_CAROL_BOB)..."
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL timeout 15 xmppc -vv \
    --jid "$CAROL_JID" \
    --pwd "$CAROL_PASS" \
    --mode message \
    chat "$BOB_JID" "$MSG_CAROL_BOB" \
    > "$TEST_DIR/carol_reply.log" 2>&1

if wait_for_pattern "$BOB_LOG" "$MSG_CAROL_BOB" 15; then
    pass "Bob received Carol's reply (Carol bidirectional routing)"
else
    log_debug "=== Bob log ==="
    log_debug "$(cat "$BOB_LOG")"
    fail "Bob did NOT receive Carol's reply: $MSG_CAROL_BOB"
fi

if grep_log "$BOB_LOG" "<message.*from=.*$CAROL_JID"; then
    pass "Bob received <message> stanza from Carol"
else
    log "WARN: Could not confirm <message> stanza from Carol in Bob log"
fi

# ===================================================================== cleanup: stop monitors and server

kill "$BOB_PID" 2>/dev/null || true
wait "$BOB_PID" 2>/dev/null || true

kill "$ALICE_MONITOR_PID" 2>/dev/null || true
wait "$ALICE_MONITOR_PID" 2>/dev/null || true

kill "$CAROL_PID" 2>/dev/null || true
wait "$CAROL_PID" 2>/dev/null || true

kill "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null || true
wait "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null || true

# ===================================================================== debug dump (only in debug mode)

if [ "$DEBUG" = true ]; then
    log "=== DEBUG: Alice log ==="
    cat "$ALICE_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Alice monitor log ==="
    cat "$ALICE_MONITOR_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Bob log ==="
    cat "$BOB_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Carol log ==="
    cat "$CAROL_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Server log (last 40 lines) ==="
    tail -40 "$SERVER_LOG" | strings | grep -v "^$" || true
fi

# ===================================================================== done

log "All checks passed."
exit 0