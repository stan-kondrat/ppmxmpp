#ifndef XEP_0059_ROSTER_VER_H
#define XEP_0059_ROSTER_VER_H

#include <stddef.h>

/* Maximum length of a SHA-256 hex digest (64 chars) + NUL. */
#define ROSTER_VER_SIZE 65

/* storage_roster_item_cb context: collect canonical string for hashing. */
typedef struct {
  char*   buf;
  size_t  len;
  size_t  cap;
  int     error;
} roster_ver_ctx_t;

/* Accumulate one roster item into the canonical string used for hashing.
 * Items MUST be processed in ascending contact_jid order so that the
 * resulting string is deterministic and reproducible. */
void roster_ver_append_item(roster_ver_ctx_t* ctx, const char* contact_jid,
                            const char* name, const char* subscription,
                            const char** groups, int gc);

/* Finalise the context and null-terminate the buffer.
 * Returns 0 on success, -1 if the buffer was too small. */
int roster_ver_finalise(roster_ver_ctx_t* ctx);

/* Compute the SHA-256 hex digest of the canonical roster string.
 * out_ver must point to a buffer of at least ROSTER_VER_SIZE bytes.
 * Returns 0 on success, -1 on error. */
int roster_ver_compute_sha256(const char* data, size_t data_len, char* out_ver);

/* Update the stored version hash for owner_jid after any roster change.
 * Rebuilds the canonical string from all current roster items and
 * stores the SHA-256 digest.
 * Returns 0 on success, -1 on error. */
int roster_ver_increment(const char* owner_jid);

/* Retrieve the stored version hash for owner_jid.
 * out_ver must point to a buffer of at least ROSTER_VER_SIZE bytes.
 * Returns 0 if a version was found, 1 if no version exists (first
 * roster use), -1 on error. */
int roster_ver_get(const char* owner_jid, char* out_ver);

/* Get the roster version without triggering an increment.
 * Useful for read-only queries (e.g. roster push construction).
 * Returns 0 if found, 1 if not found, -1 on error. */
int roster_ver_peek(const char* owner_jid, char* out_ver);

#endif /* XEP_0059_ROSTER_VER_H */