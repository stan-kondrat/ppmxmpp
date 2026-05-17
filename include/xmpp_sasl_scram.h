#ifndef XMPP_SASL_SCRAM_H
#define XMPP_SASL_SCRAM_H

#include <stddef.h>
#include <stdint.h>

/* Forward declaration — xmpp.h defines xmpp_session_t after including this header. */
typedef struct xmpp_session xmpp_session_t;

/* SCRAM-SHA-256 state for multi-round authentication (RFC 5802 + RFC 7677). */
typedef struct {
  char username[1024];         /* authcid from client-first-message-bare */
  char client_nonce[128];      /* random nonce chosen by client */
  char server_nonce[256];      /* combined nonce (client + server parts) */
  char client_first_bare[512]; /* saved for AuthMessage computation */
  char server_first[512];      /* server-first-message for AuthMessage */
  char auth_message[2048];     /* client-first-bare + server-first + client-final-without-proof */
  int has_server_first;        /* flag: server-first-message received? */
  int has_client_final;        /* flag: client-final-message received? */
  int iteration_count;         /* from server-first (s default: 4096) */
  char salt_base64[128];       /* base64-encoded salt from server-first */
  uint8_t salt[64];           /* decoded salt (max 48 bytes per RFC 5802) */
  size_t salt_len;
  uint8_t stored_key[32];     /* H(ClientKey) stored in DB */
  uint8_t server_key[32];     /* HMAC(SaltedPassword, "Server Key") stored in DB */
  uint8_t server_signature[32]; /* computed ServerSignature for server-final */
} scram_state_t;

/* ------------------------------------------------------------------ */
/*  High-level SCRAM-SHA-256 handler                                  */
/*  Called repeatedly from xmpp_feed for the same SASL session       */
/*  until the state transitions to AUTHENTICATED or terminal error.     */
/* ------------------------------------------------------------------ */

/* Handle a SCRAM-SHA-256 authentication exchange.
 * ctx:     XMPP session (contains domain, authcid, scram_state)
 * step:    current step (1=client-first, 2=client-final, 3=server-final-ack)
 * input:   base64-decoded message from client (or empty for server-triggered)
 * in_len:  byte length of input
 * response_out: NULL (server sends directly via write_fn); or for final ack pass NULL
 * write_fn: synchronous write callback
 * ud:      user data for write_fn
 *
 * Returns:  0 = success (SASL success sent), keep connection open
 *          1 = challenge sent (SASL continue), wait for client response
 *         -1 = authentication failed (wrong password/user), send failure, close
 *         -2 = terminal error (malformed, protocol violation), send failure, close
 */
int handle_scram_sha256(xmpp_session_t* ctx, int step, const char* input, size_t in_len,
                        char* response_out, size_t response_cap,
                        void (*write_fn)(void*, const char*, size_t), void* ud);

/* ------------------------------------------------------------------ */
/*  Low-level SCRAM primitives (also used by storage_users for        */
/*  generating credentials on user creation)                         */
/* ------------------------------------------------------------------ */

/* Normalize a UTF-8 password using SASLprep (RFC 4013).
 * Returns newly allocated normalized string, or NULL on failure.
 * On failure, the password contains forbidden codepoints or is invalid UTF-8. */
char* scram_saslprep(const char* password, size_t password_len);

/* Compute SaltedPassword = Hi(Normalize(password), salt, i) using SHA-256.
 * password:   SASLprep-normalized password (null-terminated)
 * salt:        random salt bytes
 * salt_len:    byte length of salt
 * iterations:  iteration count (≥ 4096 per RFC 7677)
 * output:      32-byte output buffer
 * Returns 0 on success, -1 on error. */
int scram_hi(const char* password, const uint8_t* salt, size_t salt_len,
             int iterations, uint8_t output[32]);

/* Compute HMAC-SHA-256.
 * key:    HMAC key bytes
 * klen:   key byte length
 * data:   message bytes
 * dlen:   message byte length
 * output: 32-byte output buffer */
void scram_hmac(const uint8_t* key, size_t klen, const uint8_t* data, size_t dlen,
                uint8_t output[32]);

/* Compute H(data) = SHA-256(data).
 * data:   message bytes
 * dlen:   message byte length
 * output: 32-byte output buffer */
void scram_hash(const uint8_t* data, size_t dlen, uint8_t output[32]);

/* Generate a random printable nonce string (no commas).
 * buf:    output buffer
 * buflen: byte capacity of buf (must be ≥ len)
 * len:    desired nonce length in bytes (ASCII printable output is ~2x) */
void scram_generate_nonce(char* buf, size_t buflen, size_t len);

/* Base64 encode binary data.
 * input:  source bytes
 * in_len: byte length of input
 * output: NULL-terminated base64 string written here (caller allocates)
 * out_cap: capacity of output buffer
 * Returns number of bytes written (excluding NUL), or -1 on overflow. */
int scram_base64_encode(const uint8_t* input, size_t in_len, char* output, size_t out_cap);

/* Base64 decode base64 string (with or without padding).
 * input:  base64 string (not necessarily null-terminated)
 * in_len: byte length of input
 * output: decoded bytes written here (caller allocates, needs ~in_len*3/4)
 * out_cap: capacity of output buffer
 * Returns number of bytes written, or -1 on invalid input. */
int scram_base64_decode(const char* input, size_t in_len, uint8_t* output, size_t out_cap);

/* Constant-time memcmp to resist timing attacks.
 * a, b:   byte buffers to compare
 * len:    byte count
 * Returns 1 if equal, 0 otherwise. */
int scram_ct_memeq(const uint8_t* a, const uint8_t* b, size_t len);

/* ------------------------------------------------------------------ */
/*  Server-side SCRAM verification                                     */
/*  Storage layer needs these to verify a client proof.               */
/* ------------------------------------------------------------------ */

/* Compute ClientSignature = HMAC(StoredKey, AuthMessage).
 * stored_key:  H(ClientKey), 32 bytes
 * auth_message: client-first-bare + "," + server-first + "," + client-final-without-proof
 * auth_msg_len: byte length of auth_message
 * output:      32-byte ClientSignature buffer */
void scram_client_signature(const uint8_t* stored_key, const char* auth_message,
                           size_t auth_msg_len, uint8_t output[32]);

/* Compute ServerSignature = HMAC(ServerKey, AuthMessage).
 * server_key:  HMAC(SaltedPassword, "Server Key"), 32 bytes
 * auth_message: as above
 * auth_msg_len: byte length
 * output:       32-byte ServerSignature buffer */
void scram_server_signature(const uint8_t* server_key, const char* auth_message,
                           size_t auth_msg_len, uint8_t output[32]);

/* Derive StoredKey = H(ClientKey) = H(HMAC(SaltedPassword, "Client Key")).
 * salted_password:  output of Hi(), 32 bytes
 * output:          32-byte StoredKey buffer */
void scram_derive_stored_key(const uint8_t* salted_password, uint8_t output[32]);

/* Derive ServerKey = HMAC(SaltedPassword, "Server Key").
 * salted_password:  output of Hi(), 32 bytes
 * output:           32-byte ServerKey buffer */
void scram_derive_server_key(const uint8_t* salted_password, uint8_t output[32]);

/* Derive AuthMessage = client-first-message-bare + "," + server-first + "," +
 *                      client-final-message-without-proof.
 * client_first_bare: n=user,r=nonce
 * server_first:     r=nonce,s=salt,i=count
 * client_final_no_proof: c=...,r=nonce
 * output:            combined auth message (caller allocates, ~2048)
 * cap:               capacity of output
 * Returns bytes written, or -1 on overflow. */
int scram_build_auth_message(const char* client_first_bare, const char* server_first,
                             const char* client_final_no_proof,
                             char* output, size_t cap);

/* Check if a username contains forbidden characters per RFC 7622 §3.3.1.
 * Returns 1 if valid, 0 if contains '"', '&', ''', '/', ':', ';', '<', '>', '@'. */
int scram_validate_username(const char* username);

/* Decode a SCRAM attribute-value pair from a message.
 * message:   raw SCRAM message bytes
 * msg_len:   byte length of message
 * attr:      single-letter attribute to find (e.g. 'n', 'r', 's', 'i', 'p', 'v', 'c')
 * out:       output buffer for value (caller allocates)
 * out_cap:   capacity of output buffer
 * Returns number of bytes written, or -1 if attr not found / overflow. */
int scram_get_attr(const char* message, size_t msg_len, char attr,
                   char* out, size_t out_cap);

/* Encode a SCRAM attribute-value pair into a buffer.
 * attr:  single-letter attribute
 * value: attribute value (not base64 unless attr is s, p, v, or c)
 * output: output buffer
 * cap:   capacity
 * Returns bytes written, or -1 on overflow. */
int scram_put_attr(char attr, const char* value, char* output, size_t cap);

#endif /* XMPP_SASL_SCRAM_H */