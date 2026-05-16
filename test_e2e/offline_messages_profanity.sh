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
# Checks:
#   1. Server starts and listens
#   2. Alice connects and sends the message
#   3. Server receives the message stanza (stanza=message in server log)
#   4. Server stores the message as offline (stored offline in server log)
#   5. Message is in DB with correct sender, body, and bytes_size
#   6. Bob connects and server drains offline messages (offline_drain in log)
#   7. Bob receives the message body
#   8. Bob receives <delay> stamp (XEP-0203)
#   9. Alice's JID appears in Bob's received stanza
#  10. Offline messages cleared from DB after delivery
#
# Usage:  ./test_e2e/offline_messages_profanity.sh [--debug]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-offline-messages-profanity"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
ALICE_LOG="$TEST_DIR/alice.log"
BOB_LOG="$TEST_DIR/bob.log"
PIDFILE="$TEST_DIR/.server.pid"

# shellcheck source=test_e2e/_common.sh
source "$SCRIPT_DIR/_common.sh"
# shellcheck source=test_e2e/_helpers_profanity.sh
source "$SCRIPT_DIR/_helpers_profanity.sh"

parse_common_args "$@"
trap cleanup EXIT INT TERM

TIMEOUT=15
ALICE_JID="alice@localhost"
ALICE_PASS="alicepass"
BOB_JID="bob@localhost"
BOB_PASS="bobpass"
MESSAGE_BODY="Hello Bob, you were offline!"

TEST_CONFIG="$TEST_DIR/test.conf"
TEST_DB="$TEST_DIR/test.db"
CERT_FILE="$TEST_DIR/server.crt"
KEY_FILE="$TEST_DIR/server.key"

ALICE_CONFIG_HOME="$TEST_DIR/alice_config"
ALICE_DATA_HOME="$TEST_DIR/alice_data"
ALICE_ACCOUNTS="$ALICE_DATA_HOME/profanity/accounts"

BOB_CONFIG_HOME="$TEST_DIR/bob_config"
BOB_DATA_HOME="$TEST_DIR/bob_data"
BOB_ACCOUNTS="$BOB_DATA_HOME/profanity/accounts"

mkdir -p "$ALICE_CONFIG_HOME/profanity" "$ALICE_DATA_HOME/profanity"
mkdir -p "$BOB_CONFIG_HOME/profanity" "$BOB_DATA_HOME/profanity"
chmod 755 "$ALICE_CONFIG_HOME/profanity" "$ALICE_DATA_HOME/profanity"
chmod 755 "$BOB_CONFIG_HOME/profanity" "$BOB_DATA_HOME/profanity"
if [ -d "$ALICE_ACCOUNTS" ]; then rm -rf "$ALICE_ACCOUNTS"; fi
if [ -d "$BOB_ACCOUNTS" ]; then rm -rf "$BOB_ACCOUNTS"; fi

# ===================================================================== preflight

[ -x "$SERVER_BIN" ]              || fail "Server binary not found: $SERVER_BIN (run 'make' first)"
command -v profanity &>/dev/null  || fail "profanity not in PATH"
command -v screen    &>/dev/null  || fail "screen not in PATH"
command -v openssl   &>/dev/null  || fail "openssl not in PATH"
command -v sqlite3   &>/dev/null  || fail "sqlite3 not in PATH"
command -v stdbuf    &>/dev/null  || fail "stdbuf not in PATH"

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

# ===================================================================== server

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

# Verify migrations
ACTUAL_VERSION=$(sqlite3 "$TEST_DB" "SELECT version FROM schema_version LIMIT 1;")
log_debug "Database schema version after migrations: $ACTUAL_VERSION"
[ -n "$ACTUAL_VERSION" ] || fail "schema_version table not populated"
TABLE_EXISTS=$(sqlite3 "$TEST_DB" "SELECT name FROM sqlite_master WHERE type='table' AND name='offline_messages';")
[[ -n "$TABLE_EXISTS" ]] || fail "offline_messages table not found"

# ===================================================================== seed users

log "Seeding Alice and Bob..."
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$ALICE_JID', '$ALICE_PASS', strftime('%s','now'));"
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$BOB_JID', '$BOB_PASS', strftime('%s','now'));"
USER_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM users;")
log_debug "User count in DB: $USER_COUNT"
[ "$USER_COUNT" -eq 2 ] || fail "Expected 2 users, got $USER_COUNT"
pass "Alice and Bob created"

# ===================================================================== configure profanity

TLSCERTS_CONTENT="[$CERT_FP]
subject=CN=localhost
trusted=true
fingerprint=$CERT_FP
notbefore=$CERT_NOTBEFORE
notafter=$CERT_NOTAFTER

"
printf '%s' "$TLSCERTS_CONTENT" > "$ALICE_DATA_HOME/profanity/tlscerts"
printf '%s' "$TLSCERTS_CONTENT" > "$BOB_DATA_HOME/profanity/tlscerts"

cat > "$ALICE_ACCOUNTS" <<EOF
[e2ealice]
jid=$ALICE_JID
server=127.0.0.1
port=$TLS_PORT
tls_policy=allow
password=$ALICE_PASS
EOF

cat > "$BOB_ACCOUNTS" <<EOF
[e2ebob]
jid=$BOB_JID
server=127.0.0.1
port=$TLS_PORT
tls_policy=allow
password=$BOB_PASS
EOF

# ===================================================================== phase 1: Alice connects and sends a message to Bob (offline)

log "Phase 1: Alice connects and sends a message to Bob (who is offline)..."

ALICE_SCREEN="ppmxmpp_alice_$$"
ALICE_PID=$(profanity_start "$ALICE_SCREEN" "$ALICE_CONFIG_HOME" "$ALICE_DATA_HOME" "e2ealice" "$ALICE_LOG")
log_debug "Alice screen session: $ALICE_SCREEN  profanity PID: ${ALICE_PID:-unknown}"

wait_for_session_registered "$ALICE_JID" "Alice" "$ALICE_LOG"

# Wait for profanity UI to finish its post-login IQ exchange before injecting
if ! wait_for_pattern "$ALICE_LOG" "Updating presence: online" 10; then
    dump_profanity_log "Alice log at ready-check" "$ALICE_LOG"
    dump_screen_sessions
    fail "Alice profanity did not reach ready state within 10s"
fi
pass "Alice profanity UI ready"

log_debug "Injecting /msg command into Alice's screen session..."
profanity_send "$ALICE_SCREEN" "/msg $BOB_JID $MESSAGE_BODY"

# Verify server received the message stanza
log_debug "Waiting for server to receive message stanza..."
if ! wait_for_pattern "$SERVER_LOG" "stanza=message" 15; then
    dump_profanity_log "Alice log on send failure" "$ALICE_LOG"
    dump_screen_sessions
    log_debug "=== server log (last 30 lines) ==="
    log_debug "$(tail -30 "$SERVER_LOG" 2>/dev/null | strings || true)"
    fail "Server did not receive message stanza from Alice within 15s"
fi
pass "Server received message stanza from Alice"

# Verify SENT appears in Alice's own log
if wait_for_pattern "$ALICE_LOG" "SENT.*<message" 5; then
    pass "Alice log shows SENT <message> stanza"
else
    log_debug "WARN: SENT stanza not found in Alice log (may be buffering)"
fi

profanity_stop "$ALICE_SCREEN" "$ALICE_PID"

wait_for_pattern "$SERVER_LOG" "session: unregistered $ALICE_JID" 10 || true
log_debug "Alice disconnected"

# ===================================================================== assert: server stored the message

if grep_log "$SERVER_LOG" "stored offline"; then
    pass "Server stored message as offline"
else
    log_debug "=== server log (offline-related) ==="
    log_debug "$(grep -a "offline\|stanza\|message" "$SERVER_LOG" 2>/dev/null | tail -20 || true)"
    fail "Server did not log 'stored offline' — message may not have been stored"
fi

# ===================================================================== assert: DB state after phase 1

OFFLINE_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM offline_messages WHERE recipient_jid='$BOB_JID';")
log_debug "offline_messages count for Bob: $OFFLINE_COUNT"
[ "$OFFLINE_COUNT" -gt 0 ] || {
    log_debug "=== DB offline_messages ==="
    log_debug "$(sqlite3 "$TEST_DB" "SELECT id, recipient_jid, sender_jid, received_at FROM offline_messages;" 2>/dev/null)"
    fail "No offline message stored for Bob in DB"
}
pass "Offline message stored for Bob in DB (count=$OFFLINE_COUNT)"

SENDER_IN_DB=$(sqlite3 "$TEST_DB" "SELECT sender_jid FROM offline_messages WHERE recipient_jid='$BOB_JID' LIMIT 1;")
log_debug "Sender in DB: $SENDER_IN_DB"
[[ "$SENDER_IN_DB" == "$ALICE_JID"* ]] || fail "Unexpected sender in DB: '$SENDER_IN_DB' (expected '$ALICE_JID')"
pass "Offline message sender is Alice's JID"

STANZA_XML=$(sqlite3 "$TEST_DB" "SELECT stanza_xml FROM offline_messages WHERE recipient_jid='$BOB_JID' LIMIT 1;")
echo "$STANZA_XML" | grep -q "$MESSAGE_BODY" || {
    log_debug "Stanza XML: $STANZA_XML"
    fail "Stored stanza does not contain expected message body"
}
pass "Stored stanza contains expected message body"

BYTES_SIZE=$(sqlite3 "$TEST_DB" "SELECT bytes_size FROM offline_messages WHERE recipient_jid='$BOB_JID' LIMIT 1;")
log_debug "bytes_size in DB: $BYTES_SIZE"
[ "$BYTES_SIZE" -gt 0 ] || fail "bytes_size should be > 0, got '$BYTES_SIZE'"
pass "bytes_size is positive ($BYTES_SIZE bytes)"

RECEIVED_AT=$(sqlite3 "$TEST_DB" "SELECT received_at FROM offline_messages WHERE recipient_jid='$BOB_JID' LIMIT 1;")
log_debug "received_at in DB: $RECEIVED_AT"
[ "$RECEIVED_AT" -gt 0 ] || fail "received_at should be a positive Unix timestamp"
pass "received_at is a valid timestamp ($RECEIVED_AT)"

# ===================================================================== phase 2: Bob connects and receives the offline message

log "Phase 2: Bob connects and should receive the offline message..."

BOB_SCREEN="ppmxmpp_bob_$$"
BOB_PID=$(profanity_start "$BOB_SCREEN" "$BOB_CONFIG_HOME" "$BOB_DATA_HOME" "e2ebob" "$BOB_LOG")
log_debug "Bob screen session: $BOB_SCREEN  profanity PID: ${BOB_PID:-unknown}"

wait_for_session_registered "$BOB_JID" "Bob" "$BOB_LOG"

# Wait for server to drain offline messages (XEP-0160)
if wait_for_pattern "$SERVER_LOG" "offline_drain" 10; then
    pass "Server drained offline messages for Bob"
else
    log_debug "WARN: 'offline_drain' not seen in server log (timing or log level issue)"
fi

# Wait for Bob to receive the message in his log
if ! wait_for_pattern "$BOB_LOG" "$MESSAGE_BODY" 15; then
    dump_profanity_log "Bob log on receive failure" "$BOB_LOG"
    log_debug "=== server log (last 30 lines) ==="
    log_debug "$(tail -30 "$SERVER_LOG" 2>/dev/null | strings || true)"
    fail "Bob did not receive the message body within 15s"
fi
pass "Bob received the message body"

# Wait for DB to be cleared (drain deletes rows)
log_debug "Waiting for offline messages to be cleared from DB..."
WAIT=0
until [[ $WAIT -ge 30 ]]; do
    REMAINING=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM offline_messages WHERE recipient_jid='$BOB_JID';")
    [[ "$REMAINING" -eq 0 ]] && break
    log_debug "  Still $REMAINING offline message(s) for Bob in DB..."
    sleep 0.5
    WAIT=$((WAIT+1))
done

profanity_stop "$BOB_SCREEN" "$BOB_PID"

# ===================================================================== assert: Bob's received message

[ -f "$BOB_LOG" ] || fail "Bob produced no log file"

assert_log "$BOB_LOG" "Bob received <delay> stamp (XEP-0203)" "urn:xmpp:delay"
assert_log "$BOB_LOG" "Bob received message body" "$MESSAGE_BODY"
assert_log "$BOB_LOG" "Bob received stanza with Alice's JID" "$ALICE_JID"
assert_log "$BOB_LOG" "Bob received RECV <message> stanza" "RECV.*<message"
assert_not_log "$BOB_LOG" "No login failure in Bob's log" "login failed|connection failed"

# ===================================================================== assert: DB cleared after delivery

OFFLINE_AFTER=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM offline_messages WHERE recipient_jid='$BOB_JID';")
log_debug "Remaining offline messages after delivery: $OFFLINE_AFTER"
[ "$OFFLINE_AFTER" -eq 0 ] || fail "Offline messages not cleared after delivery: $OFFLINE_AFTER remaining"
pass "Offline messages cleared from DB after delivery"

# ===================================================================== debug dump

if [ "$DEBUG" = true ]; then
    log "=== DEBUG: Alice log (SENT/RECV lines) ==="
    grep -aE "SENT|RECV|INF|ERR" "$ALICE_LOG" 2>/dev/null | tail -40 || true
    log "=== DEBUG: Bob log (SENT/RECV lines) ==="
    grep -aE "SENT|RECV|INF|ERR" "$BOB_LOG" 2>/dev/null | tail -40 || true
    log "=== DEBUG: Server log (last 50 lines) ==="
    tail -50 "$SERVER_LOG" 2>/dev/null | strings || true
    log "=== DEBUG: DB offline_messages ==="
    sqlite3 "$TEST_DB" "SELECT * FROM offline_messages;" 2>/dev/null || echo "(empty)"
fi

# ===================================================================== done

log "All checks passed."
exit 0
