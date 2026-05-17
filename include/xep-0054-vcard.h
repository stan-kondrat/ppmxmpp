#ifndef XEP_0054_VCARD_H
#define XEP_0054_VCARD_H

/* XEP-0054: vCard-temp
 * https://xmpp.org/extensions/xep-0054.html
 *
 * §Use Cases:
 *   - IQ get  → fetch stored vCard XML for a JID (or own vCard if no 'to')
 *   - IQ set  → store/replace vCard XML for the authenticated user
 *
 * Security (XEP-0054 §Viewing Another User's vCard):
 *   - Querying another user's vCard on a non-existent account MUST return
 *     service-unavailable (same error as existing user to prevent enumeration).
 *   - Attempting to set another user's vCard → forbidden/not-allowed.
 *
 * Init function: register IQ handlers for the vcard-temp namespace.
 * Returns 0 on success, -1 on error.
 */
int xep0054_init(void);

#endif /* XEP_0054_VCARD_H */