#!/usr/bin/env bash
#
# E2E test: profanity connects, authenticates, binds a resource, and the
# bound JID including the resource is visible in the profanity log.
#
# Checks:
#   1. profanity reports "logged in successfully"
#   2. The logged-in JID has the form  user@domain/<resource>  (resource present)
#   3. The server log confirms the same bound JID
#
# Usage:  ./test_e2e/profanity_bind.sh [--debug]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-profanity-bind"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
CLIENT_LOG="$TEST_DIR/profanity.log"
PIDFILE="$TEST_DIR/.server.pid"

# shellcheck source=test_e2e/_common.sh
source "$SCRIPT_DIR/_common.sh"

parse_common_args "$@"
trap cleanup EXIT INT TERM

TIMEOUT=15
TEST_JID="testuser@localhost"
TEST_PASS="testpass"

TEST_CONFIG="$TEST_DIR/test.conf"
TEST_DB="$TEST_DIR/test.db"
CERT_FILE="$TEST_DIR/server.crt"
KEY_FILE="$TEST_DIR/server.key"

PROF_CONFIG_HOME="$TEST_DIR/prof_config"
PROF_DATA_HOME="$TEST_DIR/prof_data"
PROF_ACCOUNTS="$PROF_DATA_HOME/profanity/accounts"

mkdir -p "$PROF_CONFIG_HOME/profanity" "$PROF_DATA_HOME/profanity"

# ------------------------------------------------------------------- preflight

[ -x "$SERVER_BIN" ]              || fail "Server binary not found: $SERVER_BIN (run 'make' first)"
command -v profanity &>/dev/null  || fail "profanity not in PATH"
command -v openssl   &>/dev/null  || fail "openssl not in PATH"
command -v sqlite3   &>/dev/null  || fail "sqlite3 not in PATH"
command -v script    &>/dev/null  || fail "script(1) not in PATH"

TLS_PORT=$(random_port)
log_debug "Using TLS port: $TLS_PORT"

# ------------------------------------------------------------------- certificate

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

# ------------------------------------------------------------------- profanity account

cat > "$PROF_DATA_HOME/profanity/tlscerts" <<EOF
[$CERT_FP]
subject=CN=localhost
trusted=true
fingerprint=$CERT_FP
notbefore=$CERT_NOTBEFORE
notafter=$CERT_NOTAFTER

EOF

cat > "$PROF_ACCOUNTS" <<EOF
[e2etest]
jid=$TEST_JID
server=127.0.0.1
port=$TLS_PORT
tls_policy=allow
password=$TEST_PASS
EOF

# ------------------------------------------------------------------- run profanity

log "Running profanity..."
script -q -c \
    "XDG_CONFIG_HOME=$PROF_CONFIG_HOME XDG_DATA_HOME=$PROF_DATA_HOME \
     profanity -a e2etest -l DEBUG -f $CLIENT_LOG" \
    /dev/null 2>/dev/null &
PROF_PID=$!

# Poll log for bind result IQ containing the full JID, or failure (max 20s).
# Profanity logs the bind result as:
#   xmpp: DBG: RECV: <iq ... type="result"><bind ...><jid>user@domain/resource</jid>...
WAIT=0
PROF_RESULT=""
until [[ $WAIT -ge 40 ]]; do
    if grep -qE 'RECV:.*<jid>[^<]+@[^<]+/[^<]+</jid>' "$CLIENT_LOG" 2>/dev/null; then
        PROF_RESULT="pass"
        break
    fi
    if grep -qiE "login failed|connection failed" "$CLIENT_LOG" 2>/dev/null; then
        PROF_RESULT="fail"
        break
    fi
    sleep 0.5
    WAIT=$((WAIT+1))
done
kill "$PROF_PID" 2>/dev/null || true
wait "$PROF_PID" 2>/dev/null || true

# ------------------------------------------------------------------- assertions

[ -f "$CLIENT_LOG" ] || fail "profanity produced no log file"

grep -q "logged in successfully" "$CLIENT_LOG" \
    || fail "profanity log shows no successful login"

if [[ "$PROF_RESULT" != "pass" ]]; then
    fail "profanity did not receive a bind result with a full JID (result=${PROF_RESULT:-timeout})"
fi

# Extract the bound JID from the bind result IQ line.
# Profanity logs:  xmpp: DBG: RECV: <iq ...><bind ...><jid>user@domain/resource</jid>...
BOUND_JID=$(grep -E 'RECV:.*<jid>[^<]+@[^<]+/[^<]+</jid>' "$CLIENT_LOG" \
    | grep -oE '<jid>[^<]+</jid>' | head -1 | sed 's|<jid>||;s|</jid>||')

[ -n "$BOUND_JID" ] \
    || fail "Could not extract bound JID from bind result in profanity log"
pass "Bound JID in bind result: $BOUND_JID"

# Verify the resource part is non-empty.
RESOURCE="${BOUND_JID##*/}"
[ -n "$RESOURCE" ] \
    || fail "Bound JID '$BOUND_JID' has no resource part"
pass "Resource part present: $RESOURCE"

# Verify the bare JID matches the account we logged in with.
BARE_JID="${BOUND_JID%/*}"
[ "$BARE_JID" = "$TEST_JID" ] \
    || fail "Bare JID mismatch: expected '$TEST_JID', got '$BARE_JID'"
pass "Bare JID matches: $BARE_JID"

# Verify the server also recorded the same bound JID in its log.
grep -qF "$BOUND_JID" "$SERVER_LOG" \
    || fail "Server log does not contain bound JID '$BOUND_JID'"
pass "Server log confirms bound JID: $BOUND_JID"

# ------------------------------------------------------------------- done

log "All checks passed."
exit 0
