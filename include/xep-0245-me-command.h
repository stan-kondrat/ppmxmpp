#ifndef XEP_0245_ME_COMMAND_H
#define XEP_0245_ME_COMMAND_H

#include "xmpp.h"

/* XEP-0245: The /me Command
 *
 * This XEP defines a purely client-side convention. The server has no special
 * handling to implement — it routes /me messages as-is, just like any other
 * message body.
 *
 * The only server-side step is to advertise "urn:xmpp:me-command:0" in the
 * disco#info feature list so clients know the server does not strip or modify
 * message bodies containing the /me prefix.
 *
 * References:
 *   - XEP-0245: https://xmpp.org/extensions/xep-0245.html
 */

/* Namespace advertised in disco#info to signal /me command support. */
#define XEP_0245_ME_COMMAND_NS "urn:xmpp:me-command:0"

#endif /* XEP_0245_ME_COMMAND_H */