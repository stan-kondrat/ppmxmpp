#ifndef STORAGE_ROSTER_H
#define STORAGE_ROSTER_H

/* Maximum lengths matching RFC 7622 JID limits.
 * Bare JID = localpart (1023) + '@' + domain (1023) = 2047 + NUL. */
#define ROSTER_JID_MAX 2048
#define ROSTER_NAME_MAX 1024

/* A single roster item returned from the database. */
typedef struct {
  char contact_jid[ROSTER_JID_MAX];
  char name[ROSTER_NAME_MAX];
  char subscription[8]; /* "none", "to", "from", "both", "remove" */
  int ask;              /* 1 = pending outbound subscription request */
} storage_roster_item_t;

/* Callback invoked once per roster item during iteration.
 * Return 0 to continue, non-zero to stop early. */
typedef int (*storage_roster_item_cb)(const storage_roster_item_t* item, const char** groups,
                                      int group_count, void* ud);

/* Iterate all items in owner's roster, calling cb for each.
 * Returns 0 on success (including empty roster), -1 on error. */
int storage_roster_list(const char* owner_jid, storage_roster_item_cb cb, void* ud);

/* Fetch a single item.
 * Returns 0 if found (item_out populated), 1 if not found, -1 on error. */
int storage_roster_get(const char* owner_jid, const char* contact_jid,
                       storage_roster_item_t* item_out);

/* Insert or update a roster item and its groups.
 * groups may be NULL when group_count is 0.
 * Returns 0 on success, -1 on error. */
int storage_roster_upsert(const char* owner_jid, const storage_roster_item_t* item,
                          const char** groups, int group_count);

/* Remove a roster item (and its groups via cascade).
 * Returns 0 on success (including not-found), -1 on error. */
int storage_roster_remove(const char* owner_jid, const char* contact_jid);

/* Fetch all group names for one roster item.
 * groups_out must point to an array of at least max_groups char* pointers;
 * each pointer is set to a statically allocated string valid until the
 * next call to this function.
 * Returns the number of groups found, or -1 on error. */
int storage_roster_get_groups(const char* owner_jid, const char* contact_jid,
                              const char** groups_out, int max_groups);

#endif /* STORAGE_ROSTER_H */
