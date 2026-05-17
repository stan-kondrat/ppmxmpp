#ifndef STORAGE_USERS_H
#define STORAGE_USERS_H

#include <stddef.h>
#include <stdint.h>

/* Represents a user record returned from the database. */
typedef struct {
  const char* jid;
  const char* password_plain;
} storage_user_t;

/* Represents a SCRAM credential record (RFC 5802 / RFC 7677). */
typedef struct {
  const char* jid;
  uint8_t stored_key[32];    /* H(ClientKey): H(HMAC(SaltedPassword, "Client Key")) */
  uint8_t server_key[32];     /* HMAC(SaltedPassword, "Server Key") */
  uint8_t salt[64];           /* random salt (max 64 bytes) */
  size_t salt_len;           /* actual salt byte length */
  int iteration_count;       /* PBKDF2 iteration count (≥ 4096) */
} storage_scram_t;

/* Look up a user by bare JID (e.g. "user@localhost").
 * Returns 0 on success (user found), 1 if not found, -1 on error.
 * On success, *user_out is populated with pointers into static storage. */
int storage_users_get_by_jid(const char* bare_jid, storage_user_t* user_out);

/* Check whether a user exists and the password matches (plain-text compare).
 * Returns 1 if valid, 0 if not, -1 on error. */
int storage_users_check_password(const char* bare_jid, const char* password);

/* Create a new user with the given JID and plain-text password.
 * Returns 0 on success, -1 on error. */
int storage_users_create(const char* bare_jid, const char* password_plain);

/* Disable (soft-delete) a user account.
 * Returns 0 on success, -1 on error. */
int storage_users_disable(const char* bare_jid);

/* Delete a user account (hard delete).
 * Returns 0 on success (user deleted or didn't exist), -1 on error. */
int storage_users_delete(const char* bare_jid);

/* ------------------------------------------------------------------ */
/*  SCRAM credential management (RFC 5802 + RFC 7677)                 */
/* ------------------------------------------------------------------ */

/* Retrieve SCRAM credentials for a user.
 * bare_jid:        bare JID (e.g. "alice@example.com")
 * stored_key_out:  32-byte buffer for StoredKey
 * server_key_out:  32-byte buffer for ServerKey
 * salt_out:        up-to-64-byte buffer for decoded salt
 * salt_len_out:    pointer to store actual salt length
 * iter_out:        pointer to store iteration count
 *
 * Returns 0 if credentials found, 1 if user has no SCRAM credentials,
 *         -1 on database error.
 */
int storage_scram_get_by_jid(const char* bare_jid,
                             uint8_t stored_key[32],
                             uint8_t server_key[32],
                             uint8_t salt[64],
                             size_t* salt_len_out,
                             int* iter_out);

/* Check whether a user has SCRAM credentials stored.
 * Returns 1 if yes, 0 if no, -1 on database error. */
int storage_scram_has_scram_credentials(const char* bare_jid);


/* Store SCRAM credentials for a user.
 * Creates or updates the SCRAM columns for the user.
 * If the user does not exist, returns -1.
 *
 * password:  plain-text password (will be SASLprep-normalized internally)
 * salt:     random salt bytes (caller-allocated, typically 16–32 bytes)
 * salt_len: byte length of salt
 * iterations: PBKDF2 iteration count (≥ 4096 per RFC 7677)
 *
 * Returns 0 on success, -1 on error.
 */
int storage_scram_set_password(const char* bare_jid, const char* password,
                                const uint8_t* salt, size_t salt_len, int iterations);

#endif /* STORAGE_USERS_H */
