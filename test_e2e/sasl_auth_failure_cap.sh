#!/usr/bin/env bash
#
# E2E test: three consecutive SASL PLAIN failures close the stream with
# <policy-violation/>.
#
# Usage:  ./test_e2e/sasl_auth_failure_cap.sh [--debug]
#
# Prerequisites:
#   - openssl, sqlite3 on PATH
#   - ppmxmpp built (make)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-sasl-auth-failure-cap"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
CLIENT_LOG="$TEST_DIR/client.log"
PIDFILE="$TEST_DIR/.server.pid"

# shellcheck source=test_e2e/_common.sh
source "$SCRIPT_DIR/_common.sh"

parse_common_args "$@"
trap cleanup EXIT INT TERM

TIMEOUT=15
TEST_JID="testuser@localhost"
TEST_PASS="testpass"
WRONG_PASS="wrongpass"

TEST_CONFIG="$TEST_DIR/test.conf"
TEST_DB="$TEST_DIR/test.db"
CERT_FILE="$TEST_DIR/server.crt"
KEY_FILE="$TEST_DIR/server.key"

# ------------------------------------------------------------------- preflight

[ -x "$SERVER_BIN" ]            || fail "Server binary not found: $SERVER_BIN (run 'make' first)"
command -v openssl &>/dev/null  || fail "openssl not in PATH"
command -v sqlite3 &>/dev/null  || fail "sqlite3 not in PATH"

TLS_PORT=$(random_port)
log_debug "Using TLS port: $TLS_PORT"

# ------------------------------------------------------------------- certificate

log "Generating self-signed certificate..."
openssl req -x509 -newkey rsa:2048 -keyout "$KEY_FILE" -out "$CERT_FILE" \
    -days 1 -nodes \
    -subj "/CN=localhost" \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
    2>/dev/null
pass "Certificate generated"

# ------------------------------------------------------------------- database

log "Seeding database..."
sqlite3 "$TEST_DB" <<'SQL'
CREATE TABLE IF NOT EXISTS schema_version (version INTEGER PRIMARY KEY);
CREATE TABLE IF NOT EXISTS users (
    jid            TEXT PRIMARY KEY,
    password_plain TEXT NOT NULL,
    created_at     INTEGER NOT NULL,
    disabled       INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS roster (
    owner_jid    TEXT NOT NULL,
    contact_jid  TEXT NOT NULL,
    name         TEXT NOT NULL DEFAULT '',
    subscription TEXT NOT NULL DEFAULT 'none',
    ask          INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (owner_jid, contact_jid)
);
CREATE TABLE IF NOT EXISTS roster_groups (
    owner_jid   TEXT NOT NULL,
    contact_jid TEXT NOT NULL,
    group_name  TEXT NOT NULL,
    PRIMARY KEY (owner_jid, contact_jid, group_name),
    FOREIGN KEY (owner_jid, contact_jid)
        REFERENCES roster(owner_jid, contact_jid) ON DELETE CASCADE
);
INSERT OR REPLACE INTO schema_version (version) VALUES (2);
SQL
sqlite3 "$TEST_DB" \
    "INSERT INTO users(jid, password_plain, created_at) VALUES('$TEST_JID', '$TEST_PASS', strftime('%s','now'));"
pass "Database seeded"

# ------------------------------------------------------------------- server

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

# ------------------------------------------------------------------- helpers

# base64-encode the SASL PLAIN credential: \0user\0password
sasl_plain_b64() {
    local user="$1" pass="$2"
    printf '\0%s\0%s' "$user" "$pass" | base64 -w0
}

# xmpp_exchange CRED_B64
# Does STARTTLS via openssl (which handles the plain-TCP stream open +
# <starttls>/<proceed> exchange automatically with -starttls xmpp), then
# sends a new stream open + SASL PLAIN auth over the established TLS tunnel.
# Prints all server output received after TLS is up.
xmpp_exchange() {
    local cred_b64="$1"
    local fifo="$TEST_DIR/xmpp_$$.fifo"
    mkfifo "$fifo"

    local stream_open='<?xml version="1.0"?><stream:stream xmlns="jabber:client" xmlns:stream="http://etherx.jabber.org/streams" to="localhost" version="1.0">'
    local auth_stanza="<auth xmlns='urn:ietf:params:xml:ns:xmpp-sasl' mechanism='PLAIN'>${cred_b64}</auth>"

    # Open write-end immediately so openssl can open the read-end without
    # blocking, then write XMPP frames after TLS handshake settles.
    (
        exec 3>"$fifo"
        log_debug "xmpp_exchange: writer opened fifo, waiting for TLS..."
        sleep 1
        log_debug "xmpp_exchange: sending stream open"
        printf '%s' "$stream_open" >&3
        sleep 0.5
        log_debug "xmpp_exchange: sending auth"
        printf '%s' "$auth_stanza" >&3
        sleep 1
        log_debug "xmpp_exchange: sending stream close"
        printf '%s' "</stream:stream>" >&3
        sleep 0.3
        log_debug "xmpp_exchange: closing fifo"
        exec 3>&-
    ) &
    local writer_pid=$!

    log_debug "xmpp_exchange: starting openssl (writer=$writer_pid)"
    timeout 10 openssl s_client \
        -connect "127.0.0.1:$TLS_PORT" \
        -CAfile "$CERT_FILE" \
        -starttls xmpp \
        -quiet \
        2>/dev/null \
        < "$fifo" || true
    log_debug "xmpp_exchange: openssl done"

    wait "$writer_pid" 2>/dev/null || true
    log_debug "xmpp_exchange: writer done"
    rm -f "$fifo"
}

# ------------------------------------------------------------------- test: single failure returns <failure>

log "Testing single wrong password yields <failure>..."
WRONG_B64=$(sasl_plain_b64 "testuser" "$WRONG_PASS")
log "  opening connection..."
RESP=$(xmpp_exchange "$WRONG_B64" 2>/dev/null || true)
log "  connection closed, checking response..."
log_debug "Single-failure response: $RESP"
echo "$RESP" | grep -q "not-authorized" \
    || fail "Expected <not-authorized/> on wrong password, got: $RESP"
pass "Single wrong password yields <not-authorized/>"

# ------------------------------------------------------------------- test: three failures close stream

log "Testing three consecutive wrong passwords close the stream..."
log "  opening connection for 3 attempts..."

# Build a payload that sends three auth attempts on one connection.
WRONG_B64=$(sasl_plain_b64 "testuser" "$WRONG_PASS")
STREAM_OPEN='<?xml version="1.0"?><stream:stream xmlns="jabber:client" xmlns:stream="http://etherx.jabber.org/streams" to="localhost" version="1.0">'
AUTH="<auth xmlns='urn:ietf:params:xml:ns:xmpp-sasl' mechanism='PLAIN'>${WRONG_B64}</auth>"

MULTI_FIFO="$TEST_DIR/xmpp_multi.fifo"
mkfifo "$MULTI_FIFO"

(
    exec 3>"$MULTI_FIFO"
    log_debug "multi-writer: fifo open, waiting for TLS..."
    sleep 1
    # RFC 6120 §6.4.6: after <failure> the client reopens the stream on the
    # same connection. The server must stay in a retryable state (not CLOSING)
    # between failures and only close with <policy-violation/> after 3 attempts.
    log_debug "multi-writer: attempt 1"
    printf '%s' "$STREAM_OPEN" >&3; sleep 0.4
    printf '%s' "$AUTH"        >&3; sleep 0.6
    log_debug "multi-writer: attempt 2"
    printf '%s' "$STREAM_OPEN" >&3; sleep 0.4
    printf '%s' "$AUTH"        >&3; sleep 0.6
    log_debug "multi-writer: attempt 3"
    printf '%s' "$STREAM_OPEN" >&3; sleep 0.4
    printf '%s' "$AUTH"        >&3; sleep 1
    log_debug "multi-writer: closing fifo"
    exec 3>&-
) &
MULTI_WRITER=$!

log_debug "multi: starting openssl (writer=$MULTI_WRITER)"
RAW_RESPONSE=$(
    timeout 15 openssl s_client \
        -connect "127.0.0.1:$TLS_PORT" \
        -CAfile "$CERT_FILE" \
        -starttls xmpp \
        -quiet \
        2>/dev/null \
        < "$MULTI_FIFO" || true
)
log_debug "multi: openssl done, waiting for writer..."
wait "$MULTI_WRITER" 2>/dev/null || true
rm -f "$MULTI_FIFO"
log "  connection closed, checking response..."
log_debug "Three-failure response: $RAW_RESPONSE"
echo "$RAW_RESPONSE" > "$CLIENT_LOG"

echo "$RAW_RESPONSE" | grep -q "not-authorized" \
    || fail "Expected at least one <not-authorized/> in response"

echo "$RAW_RESPONSE" | grep -q "policy-violation" \
    || fail "Expected <policy-violation/> stream close after 3 failures, got: $RAW_RESPONSE"

pass "Three failures close stream with <policy-violation/>"

# ------------------------------------------------------------------- test: correct auth still works

log "Testing correct credentials still authenticate successfully..."
GOOD_B64=$(sasl_plain_b64 "testuser" "$TEST_PASS")
log "  opening connection..."
GOOD_RESP=$(xmpp_exchange "$GOOD_B64" 2>/dev/null || true)
log "  connection closed, checking response..."
log_debug "Good-auth response: $GOOD_RESP"
echo "$GOOD_RESP" | grep -q "success" \
    || fail "Expected <success/> on correct credentials, got: $GOOD_RESP"
pass "Correct credentials yield <success/>"

# ------------------------------------------------------------------- done

log "All checks passed."
exit 0
