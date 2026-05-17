/* XEP-0245: The /me Command
 *
 * This XEP defines a purely client-side convention. The server does not
 * transform or special-case /me messages — it routes them as-is.
 *
 * The only server-side step is to advertise "urn:xmpp:me-command:0" in
 * disco#info (done in xep-0030-service-discovery.c server_features[]).
 *
 * References:
 *   - XEP-0245: https://xmpp.org/extensions/xep-0245.html
 *   - Plan: docs/plan/step-29-me-command.md
 */
#include "xep-0245-me-command.h"

