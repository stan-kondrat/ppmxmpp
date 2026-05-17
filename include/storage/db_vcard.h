#ifndef STORAGE_DB_VCARD_H
#define STORAGE_DB_VCARD_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  vCard storage                                                      */
/* ------------------------------------------------------------------ */

/* Fetch the vCard XML for a given bare JID (e.g. "alice@localhost").
 * On success (vCard exists) returns 0 and *xml_out points to a malloc'd
 * copy that the caller must free().
 * If no vCard is found returns 1 (not an error).
 * On actual error returns -1. */
int storage_vcard_get(const char* bare_jid, char** xml_out);

/* Store (insert or replace) the vCard XML for a given bare JID.
 * The entire vCard element and its XML content are stored as-is.
 * Returns 0 on success, -1 on error. */
int storage_vcard_set(const char* bare_jid, const char* vcard_xml);

/* Delete the vCard for a given bare JID.
 * Returns 0 on success (including no vCard existed), -1 on error. */
int storage_vcard_delete(const char* bare_jid);

#endif /* STORAGE_DB_VCARD_H */