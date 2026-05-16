#ifndef STORAGE_OFFLINE_H
#define STORAGE_OFFLINE_H

#include <stddef.h>

/* Maximum number of offline messages per user. */
#define OFFLINE_MAX_MESSAGES 100

/* Maximum total bytes of offline messages per user. */
#define OFFLINE_MAX_BYTES (1024 * 1024) /* 1 MB */

/* Store a message for an offline recipient.
 * Returns 0 on success, -1 on error.
 * If the user has reached the cap (100 messages or 1MB), returns -2. */
int offline_store(const char* recipient_bare_jid, const char* sender_jid, const char* stanza_xml,
                  size_t stanza_len);

/* Count offline messages for a recipient.
 * Returns the count, or -1 on error. */
int offline_count(const char* recipient_bare_jid);

/* Get total bytes of offline messages for a recipient.
 * Returns the total size, or -1 on error. */
long long offline_total_bytes(const char* recipient_bare_jid);

/* Check if storage is capped for a recipient.
 * Returns 1 if capped, 0 if not capped, -1 on error. */
int offline_is_capped(const char* recipient_bare_jid);

/* List offline messages for a recipient in received_at order.
 * Calls callback for each message.
 * callback(recipient_jid, sender_jid, stanza_xml, received_at, ud)
 * Returns 0 on success, -1 on error. */
typedef void (*offline_msg_cb)(const char* recipient_jid, const char* sender_jid,
                               const char* stanza_xml, long long received_at, void* ud);
int offline_list(const char* recipient_bare_jid, offline_msg_cb cb, void* ud);

/* Delete a single offline message by id.
 * Returns 0 on success, -1 on error. */
int offline_delete(long long id);

/* Delete all offline messages for a recipient.
 * Returns the number deleted, or -1 on error. */
int offline_delete_all(const char* recipient_bare_jid);

/* Drain all offline messages for a user and send them via write_fn.
 * Adds <delay> stamp per XEP-0203 before sending.
 * Returns 0 on success, -1 on error. */
int offline_drain(const char* recipient_bare_jid,
                  int (*write_fn)(void* ud, const char* data, size_t len), void* write_ud);

#endif /* STORAGE_OFFLINE_H */