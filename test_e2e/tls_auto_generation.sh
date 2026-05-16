#!/usr/bin/env bash
#
# E2E test: ppmxmpp auto-generates a self-signed certificate when TLS is
# enabled but no cert/key files exist.
#
# Verifies:
#   - cert and key files are created on first start
#   - certificate has CN=localhost and SAN DNS:localhost
#
# Usage:  ./test_e2e/tls_auto_generation.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - ppmxmpp built (make)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-tls-autogen"
SERVER_BIN="$PROJECT_ROOT/build/debug/ppmxmpp"
TEST_DIR=$(mktemp -d)
SERVER_LOG="$TEST_DIR/server.log"
CLIENT_LOG=""
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

# ------------------------------------------------------------------- preflight

if [ ! -x "$SERVER_BIN" ]; then
    fail "Server binary not found or not executable: $SERVER_BIN"
fi

if ! command -v openssl &>/dev/null; then
    fail "openssl not found on PATH"
fi

TLS_PORT=$(random_port)
log_debug "Using TLS port: $TLS_PORT"

# ------------------------------------------------------------------- start server (no pre-existing cert)

log "Starting ppmxmpp — no cert/key files present..."

cat > "$TEST_CONFIG" <<EOF
log_level = "DEBUG";
db_path = "$TEST_DB";
bind_host = "127.0.0.1";
bind_port = $TLS_PORT;
tls_cert_file = "$CERT_FILE";
tls_key_file = "$KEY_FILE";
EOF

start_server "$TEST_CONFIG" "$SERVER_LOG" "$PIDFILE"
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

# Stop server so stumpless flushes its log buffer to disk.
stop_server

# ------------------------------------------------------------------- verify cert and key files

if [ -f "$CERT_FILE" ]; then
    pass "Certificate file created: $CERT_FILE"
else
    fail "Certificate file was not created"
fi

if [ -f "$KEY_FILE" ]; then
    pass "Key file created: $KEY_FILE"
else
    fail "Key file was not created"
fi

# ------------------------------------------------------------------- verify cert content

CERT_TEXT=$(openssl x509 -in "$CERT_FILE" -noout -text 2>/dev/null)

if echo "$CERT_TEXT" | grep -qi "subject.*CN\s*=\s*localhost"; then
    pass "Certificate CN=localhost"
else
    fail "Certificate does not have CN=localhost"
fi

if echo "$CERT_TEXT" | grep -qi "DNS:localhost"; then
    pass "Certificate SAN contains DNS:localhost"
else
    fail "Certificate SAN does not contain DNS:localhost"
fi

# ------------------------------------------------------------------- verify server log

if grep -qi "generating self-signed\|cert.*missing\|missing.*cert" "$SERVER_LOG" 2>/dev/null; then
    pass "Server log confirms auto-generation was triggered"
else
    log_debug "Note: server log did not mention cert generation (may be a log-flush timing issue)"
fi

# ------------------------------------------------------------------- done

log "All checks passed."
exit 0
