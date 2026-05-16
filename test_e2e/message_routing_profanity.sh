#!/usr/bin/env bash
#
# E2E test: Message routing (RFC 6121 §8) with profanity
#
# Scenario:
#   1. Bob connects with profanity (stays online)
#   2. Alice connects and sends a message to Bob's bare JID → Bob receives
#   3. Alice sends a message to Bob's full JID → Bob receives
#   4. Bob replies to Alice → Alice receives (bidirectional)
#   5. Carol connects with profanity (stays online)
#   6. Alice sends to Carol → Carol receives
#   7. Bob sends to Carol → Carol receives
#   8. Carol replies to Bob → Bob receives
#
# Usage:  ./test_e2e/message_routing_profanity.sh [--debug]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-message-routing-profanity"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
ALICE_LOG="$TEST_DIR/alice.log"
BOB_LOG="$TEST_DIR/bob.log"
CAROL_LOG="$TEST_DIR/carol.log"
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
CAROL_JID="carol@localhost"
CAROL_PASS="carolpass"

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

ALICE_CONFIG_HOME="$TEST_DIR/alice_config"
ALICE_DATA_HOME="$TEST_DIR/alice_data"
ALICE_ACCOUNTS="$ALICE_DATA_HOME/profanity/accounts"

BOB_CONFIG_HOME="$TEST_DIR/bob_config"
BOB_DATA_HOME="$TEST_DIR/bob_data"
BOB_ACCOUNTS="$BOB_DATA_HOME/profanity/accounts"

CAROL_CONFIG_HOME="$TEST_DIR/carol_config"
CAROL_DATA_HOME="$TEST_DIR/carol_data"
CAROL_ACCOUNTS="$CAROL_DATA_HOME/profanity/accounts"

mkdir -p "$ALICE_CONFIG_HOME/profanity" "$ALICE_DATA_HOME/profanity"
mkdir -p "$BOB_CONFIG_HOME/profanity" "$BOB_DATA_HOME/profanity"
mkdir -p "$CAROL_CONFIG_HOME/profanity" "$CAROL_DATA_HOME/profanity"
chmod 755 "$ALICE_CONFIG_HOME/profanity" "$ALICE_DATA_HOME/profanity"
chmod 755 "$BOB_CONFIG_HOME/profanity" "$BOB_DATA_HOME/profanity"
chmod 755 "$CAROL_CONFIG_HOME/profanity" "$CAROL_DATA_HOME/profanity"

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

# ===================================================================== seed users

log "Seeding Alice, Bob, and Carol..."
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$ALICE_JID', '$ALICE_PASS', strftime('%s','now'));"
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$BOB_JID', '$BOB_PASS', strftime('%s','now'));"
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$CAROL_JID', '$CAROL_PASS', strftime('%s','now'));"
USER_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM users;")
log_debug "User count in DB: $USER_COUNT"
[ "$USER_COUNT" -eq 3 ] || fail "Expected 3 users, got $USER_COUNT"
pass "Alice, Bob, and Carol created"

# ===================================================================== configure profanity clients

TLSCERTS_CONTENT="[$CERT_FP]
subject=CN=localhost
trusted=true
fingerprint=$CERT_FP
notbefore=$CERT_NOTBEFORE
notafter=$CERT_NOTAFTER

"
printf '%s' "$TLSCERTS_CONTENT" > "$ALICE_DATA_HOME/profanity/tlscerts"
printf '%s' "$TLSCERTS_CONTENT" > "$BOB_DATA_HOME/profanity/tlscerts"
printf '%s' "$TLSCERTS_CONTENT" > "$CAROL_DATA_HOME/profanity/tlscerts"

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

cat > "$CAROL_ACCOUNTS" <<EOF
[e2ecarol]
jid=$CAROL_JID
server=127.0.0.1
port=$TLS_PORT
tls_policy=allow
password=$CAROL_PASS
EOF

# ===================================================================== helper: wait_for_profanity_ready
# Wait for profanity to finish its post-login IQ exchange and show "online".
wait_for_profanity_ready() {
    local name="$1" log_file="$2"
    if wait_for_pattern "$log_file" "Updating presence: online" 10; then
        pass "$name profanity UI ready"
    else
        dump_profanity_log "$name log at ready-check" "$log_file"
        dump_screen_sessions
        fail "$name profanity did not reach ready state within 10s"
    fi
}

# ===================================================================== phase 1: Bob connects (stays online throughout)

log "Phase 1: Bob connects with profanity..."

BOB_SCREEN="ppmxmpp_bob_$$"
BOB_PID=$(profanity_start "$BOB_SCREEN" "$BOB_CONFIG_HOME" "$BOB_DATA_HOME" "e2ebob" "$BOB_LOG")
log_debug "Bob screen: $BOB_SCREEN  PID: ${BOB_PID:-unknown}"

wait_for_session_registered "$BOB_JID" "Bob" "$SERVER_LOG"
wait_for_profanity_ready "Bob" "$BOB_LOG"

BOB_FULL_JID=$(grep -oE "bob@localhost/[a-f0-9]+" "$BOB_LOG" 2>/dev/null | head -1 || echo "")
if [ -n "$BOB_FULL_JID" ]; then
    pass "Bob's full JID: $BOB_FULL_JID"
else
    log "WARN: Could not extract Bob's full JID — full-JID routing phase will be skipped"
fi

# ===================================================================== phase 2: Alice → Bob bare JID

log "Phase 2: Alice connects and sends to Bob's bare JID (body: $MSG_BARE)..."

ALICE_SCREEN="ppmxmpp_alice_$$"
ALICE_PID=$(profanity_start "$ALICE_SCREEN" "$ALICE_CONFIG_HOME" "$ALICE_DATA_HOME" "e2ealice" "$ALICE_LOG")
log_debug "Alice screen: $ALICE_SCREEN  PID: ${ALICE_PID:-unknown}"

wait_for_session_registered "$ALICE_JID" "Alice" "$SERVER_LOG"
wait_for_profanity_ready "Alice" "$ALICE_LOG"

log_debug "Alice sending to Bob's bare JID..."
profanity_send "$ALICE_SCREEN" "/msg $BOB_JID $MSG_BARE"

if ! wait_for_pattern "$BOB_LOG" "$MSG_BARE" 20; then
    dump_profanity_log "Alice log on phase-2 failure" "$ALICE_LOG"
    dump_profanity_log "Bob log on phase-2 failure" "$BOB_LOG"
    dump_screen_sessions
    fail "Bob did not receive bare-JID message from Alice"
fi
pass "Bob received message (bare-JID routing): $MSG_BARE"

# Asserts: server side
if grep_log "$SERVER_LOG" "stanza=message"; then
    pass "Server received message stanza (phase 2)"
else
    fail "Server did not log stanza=message (phase 2)"
fi

# Asserts: Bob's received stanza
assert_log "$BOB_LOG" "Bob received RECV <message>" "RECV.*<message"
assert_log "$BOB_LOG" "Bob received stanza with Alice's JID" "$ALICE_JID"
assert_log "$BOB_LOG" "Bob received message body (bare-JID)" "$MSG_BARE"
assert_not_log "$BOB_LOG" "No connection error in Bob's log" "login failed|connection failed"

# ===================================================================== phase 3: Alice → Bob full JID

if [ -n "$BOB_FULL_JID" ]; then
    log "Phase 3: Alice sends to Bob's full JID (body: $MSG_FULL)..."

    profanity_send "$ALICE_SCREEN" "/msg $BOB_FULL_JID $MSG_FULL"

    if ! wait_for_pattern "$BOB_LOG" "$MSG_FULL" 20; then
        dump_profanity_log "Alice log on phase-3 failure" "$ALICE_LOG"
        dump_profanity_log "Bob log on phase-3 failure" "$BOB_LOG"
        fail "Bob did not receive full-JID message from Alice"
    fi
    pass "Bob received message (full-JID routing): $MSG_FULL"

    assert_log "$BOB_LOG" "Bob received message body (full-JID)" "$MSG_FULL"
else
    log "Phase 3: SKIPPED (Bob's full JID not available)"
fi

# Alice stays connected for phase 3; disconnect her after
profanity_stop "$ALICE_SCREEN" "$ALICE_PID"
wait_for_pattern "$SERVER_LOG" "session: unregistered $ALICE_JID" 10 || true

# ===================================================================== phase 4: Bob → Alice (bidirectional)

log "Phase 4: Alice reconnects, Bob replies (bidirectional)..."

ALICE_LOG2="$TEST_DIR/alice2.log"
ALICE_SCREEN2="ppmxmpp_alice2_$$"
ALICE_PID2=$(profanity_start "$ALICE_SCREEN2" "$ALICE_CONFIG_HOME" "$ALICE_DATA_HOME" "e2ealice" "$ALICE_LOG2")
log_debug "Alice2 screen: $ALICE_SCREEN2  PID: ${ALICE_PID2:-unknown}"

wait_for_session_registered "$ALICE_JID" "Alice (phase 4)" "$SERVER_LOG"
wait_for_profanity_ready "Alice (phase 4)" "$ALICE_LOG2"

log_debug "Bob sending reply to Alice..."
profanity_send "$BOB_SCREEN" "/msg $ALICE_JID $MSG_REPLY"

if ! wait_for_pattern "$ALICE_LOG2" "$MSG_REPLY" 20; then
    dump_profanity_log "Alice2 log on phase-4 failure" "$ALICE_LOG2"
    dump_profanity_log "Bob log on phase-4 failure" "$BOB_LOG"
    dump_screen_sessions
    fail "Alice did not receive Bob's reply"
fi
pass "Alice received Bob's reply (bidirectional routing): $MSG_REPLY"

assert_log "$ALICE_LOG2" "Alice received RECV <message>" "RECV.*<message"
assert_log "$ALICE_LOG2" "Alice received stanza with Bob's JID" "$BOB_JID"
assert_log "$ALICE_LOG2" "Alice received message body (reply)" "$MSG_REPLY"
assert_not_log "$ALICE_LOG2" "No connection error in Alice's log (phase 4)" "login failed|connection failed"

# Verify server logged the message stanza for this direction too
if grep_log "$SERVER_LOG" "stanza=message"; then
    pass "Server received message stanza (phase 4)"
fi

profanity_stop "$ALICE_SCREEN2" "$ALICE_PID2"
wait_for_pattern "$SERVER_LOG" "session: unregistered $ALICE_JID" 10 || true

# ===================================================================== phase 5: Carol connects (stays online)

log "Phase 5: Carol connects with profanity..."

CAROL_SCREEN="ppmxmpp_carol_$$"
CAROL_PID=$(profanity_start "$CAROL_SCREEN" "$CAROL_CONFIG_HOME" "$CAROL_DATA_HOME" "e2ecarol" "$CAROL_LOG")
log_debug "Carol screen: $CAROL_SCREEN  PID: ${CAROL_PID:-unknown}"

wait_for_session_registered "$CAROL_JID" "Carol" "$SERVER_LOG"
wait_for_profanity_ready "Carol" "$CAROL_LOG"

# ===================================================================== phase 6: Alice → Carol

log "Phase 6: Alice connects and sends to Carol (body: $MSG_ALICE_CAROL)..."

ALICE_LOG3="$TEST_DIR/alice3.log"
ALICE_SCREEN3="ppmxmpp_alice3_$$"
ALICE_PID3=$(profanity_start "$ALICE_SCREEN3" "$ALICE_CONFIG_HOME" "$ALICE_DATA_HOME" "e2ealice" "$ALICE_LOG3")
log_debug "Alice3 screen: $ALICE_SCREEN3  PID: ${ALICE_PID3:-unknown}"

wait_for_session_registered "$ALICE_JID" "Alice (phase 6)" "$SERVER_LOG"
wait_for_profanity_ready "Alice (phase 6)" "$ALICE_LOG3"

log_debug "Alice sending to Carol..."
profanity_send "$ALICE_SCREEN3" "/msg $CAROL_JID $MSG_ALICE_CAROL"

if ! wait_for_pattern "$CAROL_LOG" "$MSG_ALICE_CAROL" 20; then
    dump_profanity_log "Alice3 log on phase-6 failure" "$ALICE_LOG3"
    dump_profanity_log "Carol log on phase-6 failure" "$CAROL_LOG"
    dump_screen_sessions
    fail "Carol did not receive Alice's message"
fi
pass "Carol received Alice's message (multi-recipient routing): $MSG_ALICE_CAROL"

assert_log "$CAROL_LOG" "Carol received RECV <message>" "RECV.*<message"
assert_log "$CAROL_LOG" "Carol received stanza with Alice's JID" "$ALICE_JID"
assert_log "$CAROL_LOG" "Carol received message body (Alice→Carol)" "$MSG_ALICE_CAROL"

profanity_stop "$ALICE_SCREEN3" "$ALICE_PID3"
wait_for_pattern "$SERVER_LOG" "session: unregistered $ALICE_JID" 10 || true

# ===================================================================== phase 7: Bob → Carol

log "Phase 7: Bob sends to Carol (body: $MSG_BOB_CAROL)..."

profanity_send "$BOB_SCREEN" "/msg $CAROL_JID $MSG_BOB_CAROL"

if ! wait_for_pattern "$CAROL_LOG" "$MSG_BOB_CAROL" 20; then
    dump_profanity_log "Bob log on phase-7 failure" "$BOB_LOG"
    dump_profanity_log "Carol log on phase-7 failure" "$CAROL_LOG"
    dump_screen_sessions
    fail "Carol did not receive Bob's message"
fi
pass "Carol received Bob's message (cross-user routing): $MSG_BOB_CAROL"

assert_log "$CAROL_LOG" "Carol received Bob's JID in stanza" "$BOB_JID"
assert_log "$CAROL_LOG" "Carol received message body (Bob→Carol)" "$MSG_BOB_CAROL"

# ===================================================================== phase 8: Carol → Bob

log "Phase 8: Carol replies to Bob (body: $MSG_CAROL_BOB)..."

profanity_send "$CAROL_SCREEN" "/msg $BOB_JID $MSG_CAROL_BOB"

if ! wait_for_pattern "$BOB_LOG" "$MSG_CAROL_BOB" 20; then
    dump_profanity_log "Carol log on phase-8 failure" "$CAROL_LOG"
    dump_profanity_log "Bob log on phase-8 failure" "$BOB_LOG"
    dump_screen_sessions
    fail "Bob did not receive Carol's reply"
fi
pass "Bob received Carol's reply (Carol bidirectional routing): $MSG_CAROL_BOB"

assert_log "$BOB_LOG" "Bob received Carol's JID in stanza" "$CAROL_JID"
assert_log "$BOB_LOG" "Bob received message body (Carol→Bob)" "$MSG_CAROL_BOB"
assert_not_log "$BOB_LOG" "No connection error in Bob's log" "login failed|connection failed"
assert_not_log "$CAROL_LOG" "No connection error in Carol's log" "login failed|connection failed"

# ===================================================================== cleanup

profanity_stop "$BOB_SCREEN" "$BOB_PID"
profanity_stop "$CAROL_SCREEN" "$CAROL_PID"

# ===================================================================== debug dump

if [ "$DEBUG" = true ]; then
    log "=== DEBUG: active screen sessions ==="
    screen -ls 2>/dev/null || true
    for lbl_log in "Alice:$ALICE_LOG" "Alice2:$ALICE_LOG2" "Alice3:$ALICE_LOG3" \
                   "Bob:$BOB_LOG" "Carol:$CAROL_LOG"; do
        lbl="${lbl_log%%:*}"; lf="${lbl_log#*:}"
        [ -f "$lf" ] || continue
        log "=== DEBUG: $lbl log (SENT/RECV/ERR lines) ==="
        grep -aE "SENT|RECV|ERR|INF" "$lf" 2>/dev/null | tail -40 || true
    done
    log "=== DEBUG: Server log (last 60 lines) ==="
    tail -60 "$SERVER_LOG" 2>/dev/null | strings || true
fi

# ===================================================================== done

log "All checks passed."
exit 0
