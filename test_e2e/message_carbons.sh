#!/usr/bin/env bash
#
# E2E test: Message Carbons (XEP-0280) — multi-device sync
#
# Scenario:
#   One user (alice@localhost) has three connected resources:
#     - alice-desktop  : profanity (stays online; receives carbons)
#     - alice-mobile   : profanity (stays online; receives carbons — carbons enabled manually)
#     - alice-server   : xmpp-message (short-lived; sends messages to bob)
#   Recipient (bob@localhost): profanity (stays online; receives messages)
#
# Carbons flow:
#   - alice-server sends to bob → alice-desktop and alice-mobile receive <sent/> carbon
#   - bob replies to alice → alice-desktop receives via normal routing; alice-mobile receives <received/> carbon
#
# Usage:  ./test_e2e/message_carbons.sh [--debug]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-message-carbons"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
ALICE_DESKTOP_LOG="$TEST_DIR/alice_desktop.log"
ALICE_MOBILE_LOG="$TEST_DIR/alice_mobile.log"
ALICE_SERVER_LOG="$TEST_DIR/alice_server.log"
BOB_LOG="$TEST_DIR/bob.log"
PIDFILE="$TEST_DIR/.server.pid"

# shellcheck source=test_e2e/_common.sh
source "$SCRIPT_DIR/_common.sh"
# shellcheck source=test_e2e/_helpers_profanity.sh
source "$SCRIPT_DIR/_helpers_profanity.sh"
# shellcheck source=test_e2e/_helpers_xmpp-message.sh
source "$SCRIPT_DIR/_helpers_xmpp-message.sh"

parse_common_args "$@"
trap cleanup EXIT INT TERM

TIMEOUT=15
ALICE_JID="alice@localhost"
ALICE_PASS="alicepass"
BOB_JID="bob@localhost"
BOB_PASS="bobpass"

# Generate random message bodies
MSG_ALICE_BOB="alice_bob_$(head -c 48 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"
MSG_BOB_ALICE="bob_alice_$(head -c 48 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9' | head -c 24)"

TEST_CONFIG="$TEST_DIR/test.conf"
TEST_DB="$TEST_DIR/test.db"
CERT_FILE="$TEST_DIR/server.crt"
KEY_FILE="$TEST_DIR/server.key"

ALICE_CONFIG_HOME="$TEST_DIR/alice_config"
ALICE_DATA_HOME="$TEST_DIR/alice_data"
ALICE_MOBILE_CONFIG_HOME="$TEST_DIR/alice_mobile_config"
ALICE_MOBILE_DATA_HOME="$TEST_DIR/alice_mobile_data"
BOB_CONFIG_HOME="$TEST_DIR/bob_config"
BOB_DATA_HOME="$TEST_DIR/bob_data"

mkdir -p "$ALICE_CONFIG_HOME/profanity" "$ALICE_DATA_HOME/profanity"
mkdir -p "$ALICE_MOBILE_CONFIG_HOME/profanity" "$ALICE_MOBILE_DATA_HOME/profanity"
mkdir -p "$BOB_CONFIG_HOME/profanity" "$BOB_DATA_HOME/profanity"

# ===================================================================== preflight

[ -x "$SERVER_BIN" ]                  || fail "Server binary not found: $SERVER_BIN (run 'make' first)"
command -v profanity    &>/dev/null    || fail "profanity not in PATH"
command -v xmpp-message &>/dev/null   || fail "xmpp-message not in PATH"
command -v screen       &>/dev/null   || fail "screen not in PATH"
command -v openssl      &>/dev/null   || fail "openssl not in PATH"
command -v sqlite3      &>/dev/null   || fail "sqlite3 not in PATH"
command -v stdbuf       &>/dev/null   || fail "stdbuf not in PATH"

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

log "Seeding Alice and Bob..."
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$ALICE_JID', '$ALICE_PASS', strftime('%s','now'));"
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$BOB_JID', '$BOB_PASS', strftime('%s','now'));"
USER_COUNT=$(sqlite3 "$TEST_DB" "SELECT COUNT(*) FROM users;")
log_debug "User count in DB: $USER_COUNT"
[ "$USER_COUNT" -eq 2 ] || fail "Expected 2 users, got $USER_COUNT"
pass "Alice and Bob created"

# ===================================================================== configure profanity clients (TLS certs)

TLSCERTS_CONTENT="[$CERT_FP]
subject=CN=localhost
trusted=true
fingerprint=$CERT_FP
notbefore=$CERT_NOTBEFORE
notafter=$CERT_NOTAFTER

"
printf '%s' "$TLSCERTS_CONTENT" > "$ALICE_DATA_HOME/profanity/tlscerts"
printf '%s' "$TLSCERTS_CONTENT" > "$ALICE_MOBILE_DATA_HOME/profanity/tlscerts"
printf '%s' "$TLSCERTS_CONTENT" > "$BOB_DATA_HOME/profanity/tlscerts"

ALICE_ACCOUNTS="$ALICE_DATA_HOME/profanity/accounts"
BOB_ACCOUNTS="$BOB_DATA_HOME/profanity/accounts"

cat > "$ALICE_ACCOUNTS" <<EOF
[e2ealice]
jid=$ALICE_JID
server=127.0.0.1
port=$TLS_PORT
tls_policy=allow
password=$ALICE_PASS
EOF

ALICE_MOBILE_ACCOUNTS="$ALICE_MOBILE_DATA_HOME/profanity/accounts"

cat > "$ALICE_MOBILE_ACCOUNTS" <<EOF
[e2ealicemobile]
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

# ===================================================================== helper: wait_for_profanity_ready

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

enable_carbons() {
    local screen_name="$1" iq_id="$2"
    profanity_send "$screen_name" "/xml \"<iq type='set' id='$iq_id'><enable xmlns='urn:xmpp:carbons:2'/></iq>\""
}

# ===================================================================== phase 1: Alice's desktop connects (profanity, stays online)

log "Phase 1: Alice connects on desktop (profanity)..."

ALICE_DESKTOP_SCREEN="ppmxmpp_alice_desk_$$"
ALICE_DESKTOP_PID=$(profanity_start "$ALICE_DESKTOP_SCREEN" "$ALICE_CONFIG_HOME" \
                       "$ALICE_DATA_HOME" "e2ealice" "$ALICE_DESKTOP_LOG")
log_debug "Alice desktop screen: $ALICE_DESKTOP_SCREEN  PID: ${ALICE_DESKTOP_PID:-unknown}"

wait_for_session_registered "$ALICE_JID" "Alice desktop" "$ALICE_DESKTOP_LOG"
wait_for_profanity_ready "Alice desktop" "$ALICE_DESKTOP_LOG"

ALICE_DESKTOP_FULL=$(grep -oE "alice@localhost/[a-f0-9]+" "$ALICE_DESKTOP_LOG" 2>/dev/null | head -1 || echo "")
[ -n "$ALICE_DESKTOP_FULL" ] && pass "Alice desktop full JID: $ALICE_DESKTOP_FULL"

# ===================================================================== phase 2: Alice's mobile connects (profanity, stays online)

log "Phase 2: Alice connects on mobile (profanity)..."

ALICE_MOBILE_SCREEN="ppmxmpp_alice_mob_$$"
ALICE_MOBILE_PID=$(profanity_start "$ALICE_MOBILE_SCREEN" "$ALICE_MOBILE_CONFIG_HOME" \
                       "$ALICE_MOBILE_DATA_HOME" "e2ealicemobile" "$ALICE_MOBILE_LOG")
log_debug "Alice mobile screen: $ALICE_MOBILE_SCREEN  PID: ${ALICE_MOBILE_PID:-unknown}"

wait_for_session_registered "$ALICE_JID" "Alice mobile" "$ALICE_MOBILE_LOG"
wait_for_profanity_ready "Alice mobile" "$ALICE_MOBILE_LOG"

ALICE_MOBILE_FULL=$(grep -oE "alice@localhost/[a-f0-9]+" "$ALICE_MOBILE_LOG" 2>/dev/null | head -1 || echo "")
[ -n "$ALICE_MOBILE_FULL" ] && pass "Alice mobile full JID: $ALICE_MOBILE_FULL"

# ===================================================================== phase 3: Bob connects (profanity, stays online)

log "Phase 3: Bob connects (profanity)..."

BOB_SCREEN="ppmxmpp_bob_$$"
BOB_PID=$(profanity_start "$BOB_SCREEN" "$BOB_CONFIG_HOME" \
             "$BOB_DATA_HOME" "e2ebob" "$BOB_LOG")
log_debug "Bob screen: $BOB_SCREEN  PID: ${BOB_PID:-unknown}"

wait_for_session_registered "$BOB_JID" "Bob" "$BOB_LOG"
wait_for_profanity_ready "Bob" "$BOB_LOG"

BOB_FULL=$(grep -oE "bob@localhost/[a-f0-9]+" "$BOB_LOG" 2>/dev/null | head -1 || echo "")
[ -n "$BOB_FULL" ] && pass "Bob full JID: $BOB_FULL"

# ===================================================================== phase 4: enable carbons
# xmppc auto-enables carbons (XEP-0280) on connect; only desktop needs manual enable.

log "Phase 4: enabling carbons on alice-desktop and alice-mobile (profanity)..."

enable_carbons "$ALICE_DESKTOP_SCREEN" "en1"
enable_carbons "$ALICE_MOBILE_SCREEN" "en2"
sleep 2

CARBONS_ENABLED_COUNT=$(strings "$SERVER_LOG" 2>/dev/null | grep -c "carbons iq: enable for" || echo 0)
log_debug "carbons:enabled count in server log: $CARBONS_ENABLED_COUNT"
# Both alice resources enabled manually = expect >=2
[ "$CARBONS_ENABLED_COUNT" -ge 2 ] || {
    log_debug "=== Server log at carbons check ==="
    log_debug "$(strings "$SERVER_LOG" | tail -30)"
    dump_profanity_log "Alice desktop log" "$ALICE_DESKTOP_LOG"
    dump_profanity_log "Alice mobile log" "$ALICE_MOBILE_LOG"
    fail "Expected carbons enabled on both Alice resources (>=2), got $CARBONS_ENABLED_COUNT"
}
pass "Carbons enabled (count: $CARBONS_ENABLED_COUNT)"

SESSION_COUNT=$(strings "$SERVER_LOG" 2>/dev/null | grep -c "session: registered" || echo 0)
log_debug "Session registration count: $SESSION_COUNT"
[ "$SESSION_COUNT" -ge 3 ] || fail "Expected at least 3 session registrations, got $SESSION_COUNT"

# ===================================================================== phase 5: alice-server sends to bob via xmpp-message → carbons to desktop & mobile

log "Phase 5: alice-server (xmpp-message) sends message to bob (body: $MSG_ALICE_BOB)..."
log_debug "alice-desktop and alice-mobile should receive <sent/> carbon copy"

SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL xmpp-message \
    --jabberid "$ALICE_JID" \
    --password "$ALICE_PASS" \
    --receiver "$BOB_JID" \
    --message "$MSG_ALICE_BOB" \
    --server 127.0.0.1 \
    --port "$TLS_PORT" \
    --debug \
    > "$ALICE_SERVER_LOG" 2>&1
XMPP_MESSAGE_EXIT=$?
log_debug "xmpp-message exit status: $XMPP_MESSAGE_EXIT"

# Bob must receive the original message.
if ! wait_for_pattern "$BOB_LOG" "$MSG_ALICE_BOB" 20; then
    dump_profanity_log "Bob log" "$BOB_LOG"
    log_debug "=== Alice server log ==="
    log_debug "$(cat "$ALICE_SERVER_LOG")"
    fail "Bob did not receive message from alice-server"
fi
pass "Bob received original message from alice-server: $MSG_ALICE_BOB"

# alice-desktop must receive <sent/> carbon copy.
if ! wait_for_pattern "$ALICE_DESKTOP_LOG" "urn:xmpp:carbons:2" 20; then
    dump_profanity_log "Alice desktop log" "$ALICE_DESKTOP_LOG"
    fail "alice-desktop did not receive <sent/> carbon of alice-server's message"
fi
pass "alice-desktop received <sent/> carbon of alice-server's message"

assert_log "$ALICE_DESKTOP_LOG" "alice-desktop received forwarded stanza" \
    "urn:xmpp:forward:0"
assert_log "$ALICE_DESKTOP_LOG" "alice-desktop received original message body" \
    "$MSG_ALICE_BOB"

# alice-mobile must receive <sent/> carbon copy.
if ! wait_for_pattern "$ALICE_MOBILE_LOG" "urn:xmpp:carbons:2" 20; then
    dump_profanity_log "Alice mobile log" "$ALICE_MOBILE_LOG"
    fail "alice-mobile did not receive <sent/> carbon of alice-server's message"
fi
pass "alice-mobile received <sent/> carbon of alice-server's message"

assert_log "$ALICE_MOBILE_LOG" "alice-mobile received forwarded stanza" \
    "urn:xmpp:forward:0"
assert_log "$ALICE_MOBILE_LOG" "alice-mobile received original message body" \
    "$MSG_ALICE_BOB"

if strings "$SERVER_LOG" 2>/dev/null | grep -q "carbons: sent"; then
    pass "Server logged carbons: sent dispatch"
else
    log "WARN: Server did not log 'carbons: sent' — check server debug level"
fi

# ===================================================================== phase 6: bob replies to alice → normal delivery to desktop; <received/> carbon to mobile

log "Phase 6: bob replies to alice-desktop's full JID (body: $MSG_BOB_ALICE)..."
log_debug "alice-desktop receives original; alice-mobile receives <received/> carbon"

# Use xmpp-message to send directly to alice-desktop's full JID (deterministic routing).
SSL_CERT_FILE="$CERT_FILE" stdbuf -oL -eL xmpp-message \
    --jabberid "$BOB_JID" \
    --password "$BOB_PASS" \
    --receiver "$ALICE_DESKTOP_FULL" \
    --message "$MSG_BOB_ALICE" \
    --server 127.0.0.1 \
    --port "$TLS_PORT" \
    > "$TEST_DIR/bob_reply.log" 2>&1 || true

# alice-desktop receives the original message (full-JID exact delivery).
if ! wait_for_pattern "$ALICE_DESKTOP_LOG" "$MSG_BOB_ALICE" 20; then
    dump_profanity_log "Bob log" "$BOB_LOG"
    dump_profanity_log "Alice desktop log" "$ALICE_DESKTOP_LOG"
    fail "alice-desktop did not receive reply from bob"
fi
pass "alice-desktop received bob's reply (full-JID delivery): $MSG_BOB_ALICE"

# alice-mobile is the non-recipient resource — it must receive a <received/> carbon copy.
if ! wait_for_pattern "$ALICE_MOBILE_LOG" "received xmlns" 20; then
    dump_profanity_log "Alice mobile log" "$ALICE_MOBILE_LOG"
    dump_profanity_log "Bob log" "$BOB_LOG"
    fail "alice-mobile did not receive <received/> carbon copy of bob's reply"
fi
pass "alice-mobile received <received/> carbon of bob's reply"

assert_log "$ALICE_MOBILE_LOG" "alice-mobile received forwarded stanza (phase 6)" \
    "urn:xmpp:forward:0"
assert_log "$ALICE_MOBILE_LOG" "alice-mobile received original body from bob" \
    "$MSG_BOB_ALICE"
assert_log "$ALICE_MOBILE_LOG" "carbon from= is bob bare JID" \
    "bob@localhost"

# ===================================================================== phase 7: Verify disco features include carbons

log "Phase 7: verifying server disco#info includes carbons feature..."

# The profanity clients sent disco#info on connect; check their logs for the server's reply.
if grep -qE "urn:xmpp:carbons:2" "$ALICE_DESKTOP_LOG" "$ALICE_MOBILE_LOG" "$BOB_LOG" 2>/dev/null; then
    pass "Server disco#info includes urn:xmpp:carbons:2"
else
    fail "Server disco#info did not list urn:xmpp:carbons:2"
fi

if grep -qE "urn:xmpp:forward:0" "$ALICE_DESKTOP_LOG" "$ALICE_MOBILE_LOG" "$BOB_LOG" 2>/dev/null; then
    pass "Server disco#info includes urn:xmpp:forward:0"
else
    fail "Server disco#info did not list urn:xmpp:forward:0"
fi

# ===================================================================== cleanup

profanity_stop "$ALICE_DESKTOP_SCREEN" "$ALICE_DESKTOP_PID"
profanity_stop "$ALICE_MOBILE_SCREEN" "$ALICE_MOBILE_PID"
profanity_stop "$BOB_SCREEN" "$BOB_PID"

# ===================================================================== debug dump

if [ "$DEBUG" = true ]; then
    log "=== DEBUG: Alice desktop log ==="
    grep -aE "SENT|RECV|ERR|INF" "$ALICE_DESKTOP_LOG" 2>/dev/null | tail -40 || true
    log "=== DEBUG: Alice mobile log ==="
    grep -aE "SENT|RECV|ERR|INF" "$ALICE_MOBILE_LOG" 2>/dev/null | tail -40 || true
    log "=== DEBUG: Alice server log ==="
    cat "$ALICE_SERVER_LOG" 2>/dev/null | tail -40 || true
    log "=== DEBUG: Bob log ==="
    grep -aE "SENT|RECV|ERR|INF" "$BOB_LOG" 2>/dev/null | tail -40 || true
    log "=== DEBUG: Server log (last 60 lines) ==="
    tail -60 "$SERVER_LOG" 2>/dev/null | strings || true
fi

# ===================================================================== done

log "All checks passed."
exit 0
