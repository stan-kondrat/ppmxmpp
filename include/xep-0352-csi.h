#ifndef XEP_0352_CSI_H
#define XEP_0352_CSI_H

#include "xmpp.h"
#include "strophe.h"
#include <stdarg.h>
#include <stdio.h>

/* XEP-0352: Client State Indication
 * https://xmpp.org/extensions/xep-0352.html
 *
 * Tracks client active/inactive state per session.
 * No reply is sent to <active/> or <inactive/> elements (XEP-0352 §4.2).
 *
 * The stream always begins in ACTIVE state (XEP-0352 §4.2).
 * CSI state is NOT persisted across stream resumption (XEP-0352 §6.3).
 */

/* CSI state values. */
typedef enum {
  XMPP_CSI_ACTIVE,   /* Default; server optimisations are disabled. */
  XMPP_CSI_INACTIVE,  /* Client is idle; server MAY apply traffic optimisations. */
} xmpp_csi_state_t;

/* Register CSI stanza handler into the ONLINE state handler dispatch.
 * Called during server init before handling connections. */
int xep0352_init(void);

/* Handle an incoming CSI <active/> or <inactive/> stanza.
 * Called from xmpp.c's on_stanza() when in XMPP_STATE_ONLINE.
 *
 * Returns 0 on success, -1 if the stanza was malformed. */
int xep0352_handle_stanza(xmpp_session_t* ctx, xmpp_stanza_t* stanza);

/* Format a <csi xmlns='urn:xmpp:csi:0'/> stream feature for advertise.
 * Appends to ctx->out_buf; ctx->out_len is advanced.
 * Returns 0 on success, -1 if the buffer is full. */
int xep0352_append_stream_feature(xmpp_session_t* ctx);

/* Helper: append formatted text to ctx->out_buf (mirrors xmpp.c's write_append). */
static inline int csi_write_append(xmpp_session_t* ctx, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(ctx->out_buf + ctx->out_len, XMPP_BUF_SIZE - ctx->out_len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= XMPP_BUF_SIZE - ctx->out_len) {
    return -1;
  }
  ctx->out_len += (size_t)n;
  return 0;
}

#endif /* XEP_0352_CSI_H */