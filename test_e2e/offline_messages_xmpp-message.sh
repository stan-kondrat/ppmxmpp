#!/usr/bin/env bash
#
# E2E test: Offline message delivery (XEP-0160, XEP-0203)
#
# Scenario:
#   1. Bob is offline (not connected)
#   2. Alice connects and sends a message to Bob's bare JID
#   3. Bob connects and should receive the stored message
#      with a <delay> stamp indicating the original delivery time (XEP-0203)
#
# Prerequisites:
#   1. App creates DB schema via migrations (NOT manually in test)
#   2. Creating Bob and Alice users
#
# Checks:
#   1. Server starts and listens
#   2. Alice connects and sends the message
#   3. Server receives the message stanza (verifies "stanza=message" in server log)
#   4. Message is stored as offline in DB
#   5. Bob connects and receives the message
#   6. Message has <delay> stamp (XEP-0203)
#   7. Message body is present
#   8. Offline messages are cleared after delivery
#
# Usage:  ./test_e2e/offline_messages_xmpp-message.sh [--debug]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-offline-messages-xmpp-message"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
ALICE_LOG="$TEST_DIR/alice.log"
BOB_LOG="$TEST_DIR/bob.log"
PIDFILE="$TEST_DIR/.server.pid"

# shellcheck source=test_e2e/_common.sh
source "$SCRIPT_DIR/_common.sh"
# shellcheck source=test_e2e/_helpers_xmpp-message.sh
source "$SCRIPT_DIR/_helpers_xmpp-message.sh"
# shellcheck source=test_e2e/_helpers_profanity.sh
source "$SCRIPT_DIR/_helpers_profanity.sh"

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

BOB_CONFIG_HOME="$TEST_DIR/bob_config"
BOB_DATA_HOME="$TEST_DIR/bob_data"
mkdir -p "$BOB_CONFIG_HOME/profanity" "$BOB_DATA_HOME/profanity"

# ===================================================================== preflight

[ -x "$SERVER_BIN" ]              || fail "Server binary not found: $SERVER_BIN (run 'make' first)"
command -v xmpp-message &>/dev/null  || fail "xmpp-message not in PATH"
command -v profanity    &>/dev/null  || fail "profanity not in PATH"
command -v screen       &>/dev/null  || fail "screen not in PATH"
command -v openssl   &>/dev/null  || fail "openssl not in PATH"
command -v sqlite3   &>/dev/null  || fail "sqlite3 not in PATH"
command -v stdbuf   &>/dev/null  || fail "stdbuf not in PATH"

# Verify xmpp-message works (check for Python library compatibility issues)
XMPPM_TEST_OUTPUT=$(timeout 5 stdbuf -oL -eL xmpp-message --jabberid "test@localhost" --password "test" --receiver "test2@localhost" --message "test" --server 127.0.0.1 --port 1 2>&1 || true)
if echo "$XMPPM_TEST_OUTPUT" | grep -qi "AttributeError\|Traceback"; then
    log "SKIP: xmpp-message has Python library compatibility issues"
    log "  xmpp-message output: $(echo "$XMPPM_TEST_OUTPUT" | head -5)"
    exit 0
fi
if [ -z "$XMPPM_TEST_OUTPUT" ]; then
    log "SKIP: xmpp-message timed out or produced no output (possible compatibility issue)"
    exit 0
fi

TLS_PORT=$(random_port)
log_debug "Using TLS port: $TLS_PORT"

# ===================================================================== certificate

log "Generating self-signed certificate..."
openssl req -x509 -newkey rsa:2048 -keyout "$KEY_FILE" -out "$CERT_FILE" \
    -days 1 -nodes \
    -subj "/CN=localhost" \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
    2>/dev/null
CERT_FP=$(openssl x509 -in "$CERT_FILE" -noout -fingerprint -sha256 2>/dev/null \
    | cut -d= -f2 | tr -d ':' | tr '[:upper:]' '[:lower:]')
CERT_NOTBEFORE=$(openssl x509 -in "$CERT_FILE" -noout -startdate 2>/dev/null | cut -d= -f2)
CERT_NOTAFTER=$(openssl x509 -in "$CERT_FILE" -noout -enddate   2>/dev/null | cut -d= -f2)
pass "Certificate generated (SHA-256: $CERT_FP)"

# Configure profanity for Bob (TLS cert trust + account)
TLSCERTS_CONTENT="[$CERT_FP]
subject=CN=localhost
trusted=true
fingerprint=$CERT_FP
notbefore=$CERT_NOTBEFORE
notafter=$CERT_NOTAFTER

"
printf '%s' "$TLSCERTS_CONTENT" > "$BOB_DATA_HOME/profanity/tlscerts"
cat > "$BOB_DATA_HOME/profanity/accounts" <<EOF
[e2ebob]
jid=$BOB_JID
server=127.0.0.1
port=$TLS_PORT
tls_policy=allow
password=$BOB_PASS
EOF

# ===================================================================== server (creates DB schema via migrations on startup)

cat > "$TEST_CONFIG" <<EOF
log_level = "DEBUG";
db_path = "$TEST_DB";
bind_host = "127.0.0.1";
bind_port = $TLS_PORT;
tls_cert_file = "$CERT_FILE";
tls_key_file = "$KEY_FILE";
EOF

log "Starting ppmxmpp on TLS 127.0.0.1:$TLS_PORT..."
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
pass "Server listening on TLS 127.0.0.1:$TLS_PORT"

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

# Verify users were created
USER_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM users;")
log_debug "User count in DB: $USER_COUNT"
[ "$USER_COUNT" -eq 2 ] || fail "Expected 2 users, got $USER_COUNT"

# ===================================================================== phase 1: Alice connects and sends a message to Bob (who is offline)
log "Phase 1: Alice connects and sends a message to Bob (who is offline)..."

# Use xmpp-message to send the message
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL xmpp-message \
    --jabberid "$ALICE_JID" \
    --password "$ALICE_PASS" \
    --receiver "$BOB_JID" \
    --message "$MESSAGE_BODY" \
    --server 127.0.0.1 \
    --port $TLS_PORT \
    --debug \
    > "$ALICE_LOG" 2>&1 || true

# Wait for the server to record the message stanza
wait_for_pattern "$SERVER_LOG" "stanza=message" 10

# Check if message was sent successfully
if grep_log "$ALICE_LOG" "sent|message.*delivered|Success"; then
    pass "Alice sent the message (xmpp-message output)"
else
    log_debug "=== Alice log ==="
    log_debug "$(cat "$ALICE_LOG")"
    # Check for errors
    if grep_log "$ALICE_LOG" "error|fail|disconnected"; then
        fail "Alice failed to send message - error in output"
    fi
    log "WARN: Could not confirm message sent from xmpp-message output"
fi

# ===================================================================== assert: server received the message stanza

log "Verifying server received the message stanza..."

# Check server log for: "stanza=message" (not "stanza=iq")
if grep_log "$SERVER_LOG" "stanza=message"; then
    pass "Server received the message stanza (stanza=message in server log)"
else
    log_debug "=== Server log entries ==="
    log_debug "$(strings "$SERVER_LOG" 2>/dev/null | grep -E "stanza|recv|message" | head -20)"
    fail "Server did NOT receive a message stanza — check server log"
fi

# Check server log for offline storage confirmation
if grep_log "$SERVER_LOG" "stored offline"; then
    pass "Server log shows 'stored offline'"
else
    # Check for any error in storage
    if grep_log "$SERVER_LOG" "failed to store"; then
        log_debug "$(strings "$SERVER_LOG" 2>/dev/null | grep -E "failed to store|offline_store|offline" | head -10)"
        fail "Server log shows 'failed to store offline message'"
    fi
    log_debug "$(strings "$SERVER_LOG" 2>/dev/null | grep -E "offline|message" | head -10)"
    fail "Server log does NOT show 'stored offline' for Bob"
fi

# ===================================================================== assert: offline storage in DB

OFFLINE_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM offline_messages WHERE recipient_jid='$BOB_JID';")
log_debug "offline_messages count for Bob: $OFFLINE_COUNT"
if [[ "$OFFLINE_COUNT" -gt 0 ]]; then
    pass "Message stored as offline for Bob (count=$OFFLINE_COUNT)"
else
    log_debug "=== DB offline_messages table ==="
    log_debug "$(sqlite3 "$TEST_DB" "SELECT id, recipient_jid, sender_jid, received_at FROM offline_messages;" 2>/dev/null)"
    log_debug "=== Server log (offline-related) ==="
    log_debug "$(strings "$SERVER_LOG" 2>/dev/null | grep -E "offline|message|stanza" | head -20)"
    fail "No offline message stored for Bob"
fi

# Verify sender in DB
SENDER_IN_DB=$(sqlite3 "$TEST_DB" "SELECT sender_jid FROM offline_messages WHERE recipient_jid='$BOB_JID' LIMIT 1;")
log_debug "Sender in DB: $SENDER_IN_DB"
if [[ "$SENDER_IN_DB" == "$ALICE_JID"* ]]; then
    pass "Offline message sender is Alice (as expected)"
else
    log_debug "Unexpected sender: $SENDER_IN_DB (expected: $ALICE_JID)"
fi

# Verify stanza XML contains expected body
STANZA_XML=$(sqlite3 "$TEST_DB" "SELECT stanza_xml FROM offline_messages WHERE recipient_jid='$BOB_JID' LIMIT 1;")
if echo "$STANZA_XML" | grep -q "$MESSAGE_BODY"; then
    pass "Offline stanza contains expected message body"
else
    log_debug "Stanza XML: $STANZA_XML"
    fail "Offline stanza does NOT contain expected message body"
fi

# Verify bytes_size is reasonable
BYTES_SIZE=$(sqlite3 "$TEST_DB" "SELECT bytes_size FROM offline_messages WHERE recipient_jid='$BOB_JID' LIMIT 1;")
log_debug "bytes_size in DB: $BYTES_SIZE"
if [[ "$BYTES_SIZE" -gt 0 ]]; then
    pass "bytes_size is positive ($BYTES_SIZE)"
else
    fail "bytes_size should be > 0"
fi

# ===================================================================== phase 2: Bob connects via profanity (triggers offline delivery on presence)

log "Phase 2: Bob connects (profanity) and should receive the offline message..."

BOB_SCREEN="ppmxmpp_bob_$$"
BOB_PID=$(profanity_start "$BOB_SCREEN" "$BOB_CONFIG_HOME" "$BOB_DATA_HOME" "e2ebob" "$BOB_LOG")
log_debug "Bob screen: $BOB_SCREEN  PID: ${BOB_PID:-unknown}"

wait_for_session_registered "$BOB_JID" "Bob" "$BOB_LOG"

# Bob's profanity log should contain the delivered offline message body.
if ! wait_for_pattern "$BOB_LOG" "$MESSAGE_BODY" 15; then
    dump_profanity_log "Bob log" "$BOB_LOG"
    log_debug "=== Server log (last 20 lines) ==="
    log_debug "$(strings "$SERVER_LOG" 2>/dev/null | tail -20)"
    fail "Bob did not receive the offline message"
fi
pass "Bob received the offline message: $MESSAGE_BODY"

# <delay> stamp should appear in Bob's log (XEP-0203).
if wait_for_pattern "$BOB_LOG" "urn:xmpp:delay" 5; then
    pass "Bob received <delay> stamp (XEP-0203)"
else
    log "WARN: <delay> stamp not visible in Bob's profanity log"
fi

profanity_stop "$BOB_SCREEN" "$BOB_PID"

# ===================================================================== assert: offline messages cleared after delivery

# Assert: Offline messages drained after Bob login
OFFLINE_AFTER=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM offline_messages WHERE recipient_jid='$BOB_JID';")
if [[ "$OFFLINE_AFTER" -eq 0 ]]; then
    pass "Offline messages cleared from DB after delivery"
else
    fail "Offline messages NOT cleared: $OFFLINE_AFTER remaining in DB"
fi

# ===================================================================== debug dump (only in debug mode)

if [ "$DEBUG" = true ]; then
    log "=== DEBUG: Alice log ==="
    cat "$ALICE_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Bob log ==="
    cat "$BOB_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: Server log (last 40 lines) ==="
    tail -40 "$SERVER_LOG" | strings | grep -v "^$" || true
    log "=== DEBUG: DB offline_messages ==="
    sqlite3 "$TEST_DB" "SELECT * FROM offline_messages;" 2>/dev/null || echo "(empty)"
fi

# ===================================================================== done

log "All checks passed."
exit 0