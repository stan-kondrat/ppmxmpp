#ifndef STORAGE_USERS_H
#define STORAGE_USERS_H

#include <stddef.h>

/* Represents a user record returned from the database. */
typedef struct {
  const char* jid;
  const char* password_plain;
} storage_user_t;

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

#endif /* STORAGE_USERS_H */
