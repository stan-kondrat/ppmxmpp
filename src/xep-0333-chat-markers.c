/* XEP-0333: Chat Markers
 * https://xmpp.org/extensions/xep-0333.html
 *
 * v1.0.0 (2024-04-17) — "Displayed Markers" (renamed from "Chat Markers")
 * §2    <markable/> — client signals it wants displayed markers for this message.
 * §3    <displayed xmlns='urn:xmpp:chat-markers:0' id='msg-id'/> — message shown
 *        to user (only remaining marker type; <received/> and <acknowledged/>
 *        were removed in v1.0.0).
 *
 * The server role is minimal: marker messages are standard <message>
 * stanzas.  This module provides stanza-inspection helpers.  Routing itself
 * is handled by xmpp_message_handle() as a normal chat message.
 *
 * XEP-0184 (Delivery Receipts) <received/> uses namespace
 * urn:xmpp:receipts while XEP-0333 uses urn:xmpp:chat-markers:0.
 * Both are routed identically; the distinction is made by namespace.
 */
#include "xep-0333-chat-markers.h"


#include <string.h>

#include "strophe.h"

/* Supported marker element names in the chat-markers namespace.
 * XEP-0333 v1.0.0 only defines <displayed/>.  <received/> and
 * <acknowledged/> are kept for backward compatibility with pre-v1.0.0
 * clients that implemented the draft versions. */
static const char* const s_marker_names[] = {"displayed", "received", "acknowledged"};

/* Marker namespace per XEP-0333 §1. */
static const char* const s_marker_ns = "urn:xmpp:chat-markers:0";

const char* xep0333_get_marker(const xmpp_stanza_t* stanza, const char** marker_id_out) {
  if (marker_id_out) *marker_id_out = NULL;
  if (!stanza) return NULL;

  /* xmpp_stanza_get_children() takes non-const; stanza is const here but
   * this is post-parse so casting away const is safe. */
  xmpp_stanza_t* child = xmpp_stanza_get_children((xmpp_stanza_t*)(uintptr_t)stanza);  // NOLINT(performance-no-int-to-ptr)
  while (child) {
    const char* name = xmpp_stanza_get_name(child);
    const char* ns = xmpp_stanza_get_ns(child);

    /* Check whether this child is a chat-marker in the right namespace. */
    if (name && ns && strcmp(ns, s_marker_ns) == 0) {
      for (size_t i = 0; i < sizeof(s_marker_names) / sizeof(s_marker_names[0]); i++) {
        if (strcmp(name, s_marker_names[i]) == 0) {
          if (marker_id_out) {
            *marker_id_out = xmpp_stanza_get_attribute(child, "id");
          }
          return name; /* caller does NOT own this pointer */
        }
      }
    }
    child = xmpp_stanza_get_next(child);
  }
  return NULL;
}

int xep0333_has_markable(const xmpp_stanza_t* stanza) {
  if (!stanza) return 0;


  xmpp_stanza_t* child = xmpp_stanza_get_children((xmpp_stanza_t*)(uintptr_t)stanza);  // NOLINT(performance-no-int-to-ptr)
  while (child) {
    const char* name = xmpp_stanza_get_name(child);
    const char* ns = xmpp_stanza_get_ns(child);
    if (name && strcmp(name, "markable") == 0 &&
        ns && strcmp(ns, s_marker_ns) == 0) {
      return 1;
    }
    child = xmpp_stanza_get_next(child);
  }
  return 0;
}