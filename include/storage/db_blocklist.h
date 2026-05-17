#ifndef STORAGE_BLOCKLIST_H
#define STORAGE_BLOCKLIST_H

#include <stddef.h>

#define BLOCKLIST_JID_MAX 2048

/* Callback invoked once per blocklist item during iteration.
 * Return 0 to continue, non-zero to stop early. */
typedef int (*storage_blocklist_cb)(const char* blocked_jid, void* ud);

/* Check if owner_jid has blocked blocked_jid.
 * Returns 1 if blocked, 0 if not blocked, -1 on error. */
int storage_blocklist_check(const char* owner_jid, const char* blocked_jid);

/* Block a JID: insert or ignore into blocklist.
 * Returns 0 on success, -1 on error. */
int storage_blocklist_add(const char* owner_jid, const char* blocked_jid);

/* Unblock a JID: remove from blocklist.
 * Returns 0 on success (including not-found), -1 on error. */
int storage_blocklist_remove(const char* owner_jid, const char* blocked_jid);

/* Iterate all blocked JIDs for owner_jid, calling cb for each.
 * Returns 0 on success, -1 on error. */
int storage_blocklist_list(const char* owner_jid, storage_blocklist_cb cb, void* ud);

#endif /* STORAGE_BLOCKLIST_H */