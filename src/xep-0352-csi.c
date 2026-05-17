/* XEP-0352: Client State Indication
 * https://xmpp.org/extensions/xep-0352.html
 *
 * §4.1  Advertise <csi xmlns='urn:xmpp:csi:0'/> in stream features (post-bind).
 * §4.2  Handle incoming <active xmlns='urn:xmpp:csi:0'/>  → CSI_ACTIVE.
 * §4.2  Handle incoming <inactive xmlns='urn:xmpp:csi:0'/> → CSI_INACTIVE.
 *
 * No server reply is required (XEP-0352 §4.2).
 * CSI state defaults to ACTIVE on session start and is NOT restored
 * after stream resumption (XEP-0352 §6.3).
 */
#include "xep-0352-csi.h"

#include <string.h>

#include "log.h"

#define CSI_NS "urn:xmpp:csi:0"

/* Handle an incoming CSI stanza (<active/> or <inactive/>).
 *
 * Matches on stanza name and namespace.  Silently ignores stanzas
 * with unexpected names or namespaces (no error per XEP-0352).
 *
 * Returns 0 on matched and processed, -1 on a protocol-level error
 * (e.g., unexpected child elements that indicate a misbehaving client). */
int xep0352_handle_stanza(xmpp_session_t* ctx, xmpp_stanza_t* stanza) {
  const char* name = xmpp_stanza_get_name(stanza);
  const char* ns   = xmpp_stanza_get_ns(stanza);

  /* Check if stanza has any child elements (XEP-0352 §4.2: error if present).
   * xmpp_stanza_get_name() returns NULL for text nodes, non-NULL for tag nodes. */
  int has_children = 0;
  xmpp_stanza_t* child = xmpp_stanza_get_children(stanza);
  while (child) {
    if (xmpp_stanza_get_name(child) != NULL) {
      has_children = 1;
      break;
    }
    child = xmpp_stanza_get_next(child);
  }

  /* CSI elements are bare top-level stanzas — no type attribute expected. */
  if (strcmp(name, "active") == 0) {
    if (ns && strcmp(ns, CSI_NS) == 0) {
      if (has_children) {
        stump_w("csi conn_id='%s' active with children, ignoring", ctx->conn_id);
        return -1;
      }
      ctx->csi_state = XMPP_CSI_ACTIVE;
      stump_d("csi conn_id='%s' state=ACTIVE", ctx->conn_id);
      return 0;
    }
  }

  if (strcmp(name, "inactive") == 0) {
    if (ns && strcmp(ns, CSI_NS) == 0) {
      if (has_children) {
        stump_w("csi conn_id='%s' inactive with children, ignoring", ctx->conn_id);
        return -1;
      }
      ctx->csi_state = XMPP_CSI_INACTIVE;
      stump_d("csi conn_id='%s' state=INACTIVE", ctx->conn_id);
      return 0;
    }
  }

  /* Not a CSI stanza — tell the caller to try another handler. */
  return 1;
}

/* Append the CSI stream feature advertisement to ctx->out_buf.
 *
 * This is called from xmpp.c's send_stream_features() in the
 * XMPP_STATE_BOUND state so clients can discover CSI support.
 *
 * Returns 0 on success, -1 if the buffer is full. */
int xep0352_append_stream_feature(xmpp_session_t* ctx) {
  return csi_write_append(ctx, "<csi xmlns='urn:xmpp:csi:0'/>");
}

/* Minimal init — no handler table needed since CSI stanzas arrive
 * as bare top-level elements dispatched directly by xmpp.c:on_stanza().
 * Provides a single entry point for future expansion. */
int xep0352_init(void) {
  stump_i("xep0352: CSI (XEP-0352) initialised");
  return 0;
}