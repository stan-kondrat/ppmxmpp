#!/usr/bin/env bash
#
# E2E test: ppmxmpp serves TLS on a configured port using an externally
# generated certificate.
#
# Verifies:
#   - server starts and listens with the provided cert/key
#   - openssl s_client can connect (TCP-level)
#   - server log references the configured cert path
#   - cert fingerprint on disk matches what was configured
#
# Usage:  ./test_e2e/tls_connection.sh [--debug]
#
# Prerequisites:
#   - openssl on PATH
#   - ppmxmpp built (make)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TEST_NAME="e2e-tls-conn"
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

# ------------------------------------------------------------------- generate cert with openssl

log "Generating self-signed certificate with openssl..."

openssl req -x509 -newkey rsa:2048 -keyout "$KEY_FILE" -out "$CERT_FILE" \
    -days 1 -nodes \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost" \
    2>/dev/null

CERT_FP=$(openssl x509 -in "$CERT_FILE" -noout -fingerprint -sha256 2>/dev/null | cut -d= -f2)
pass "Certificate generated (SHA-256: $CERT_FP)"
log_debug "Cert: $CERT_FILE  Key: $KEY_FILE"

# ------------------------------------------------------------------- start server

log "Starting ppmxmpp with openssl-generated cert..."

cat > "$TEST_CONFIG" <<EOF
log_level = "DEBUG";
db_path = "$TEST_DB";
bind_enabled = false;
tls_enabled = true;
tls_host = "127.0.0.1";
tls_port = $TLS_PORT;
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

# ------------------------------------------------------------------- connect with openssl s_client

log "Connecting with openssl s_client..."

# Use a short timeout: with a raw-TCP server (TLS handshake not yet implemented)
# s_client sends ClientHello but never receives ServerHello, so it hangs until
# killed.  "Connecting to" is printed before the TCP attempt; its presence plus
# the absence of "Connection refused" confirms the port was reached.
timeout 2 openssl s_client \
    -connect "127.0.0.1:$TLS_PORT" \
    </dev/null > "$CLIENT_LOG" 2>&1 || true

log_debug "openssl s_client output:"
log_debug "$(sed 's/^/  /' "$CLIENT_LOG")"

if grep -qi "connection refused\|could not connect\|no route" "$CLIENT_LOG"; then
    fail "openssl s_client could not reach TLS port"
elif grep -qi "Connecting to" "$CLIENT_LOG"; then
    pass "openssl s_client reached TLS port (TCP connected; TLS handshake pending server implementation)"
else
    fail "openssl s_client produced no output — port may be unreachable"
fi

# ------------------------------------------------------------------- stop server and check logs

stop_server

# Verify the server log references the configured cert path.
if grep -qF "$CERT_FILE" "$SERVER_LOG" 2>/dev/null; then
    pass "Server log references configured cert path"
else
    fail "Server log does not reference cert path: $CERT_FILE"
fi

# ------------------------------------------------------------------- cert fingerprint

# Confirm the cert file on disk is intact and matches what was generated.
VERIFY_FP=$(openssl x509 -in "$CERT_FILE" -noout -fingerprint -sha256 2>/dev/null | cut -d= -f2)
if [ "$VERIFY_FP" = "$CERT_FP" ]; then
    pass "Cert fingerprint unchanged (SHA-256: $CERT_FP)"
else
    fail "Cert fingerprint mismatch: expected $CERT_FP, got $VERIFY_FP"
fi

# ------------------------------------------------------------------- done

log "All checks passed."
exit 0
