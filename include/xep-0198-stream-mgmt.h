/* XEP-0198: Stream Management
 * https://xmpp.org/extensions/xep-0198.html
 *
 * Provides:
 *   - Stanza acknowledgements (<r/>, <a h='N'/>)
 *   - Stream resumption (<resume/>, <resumed/>)
 *   - SM-ID generation (HMAC-SHA256 over authcid + stream_id)
 *
 * Integrate with xmpp.c:
 *   - send_stream_features() advertises <sm xmlns='urn:xmpp:sm:3'/>
 *     after resource binding (XMPP_STATE_BOUND).
 *   - on_stanza() routes SM top-level elements to handle_sm_element().
 */

#ifndef XEP_0198_STREAM_MGMT_H
#define XEP_0198_STREAM_MGMT_H

#include "xmpp.h"
#include "strophe.h"
#include <stdarg.h>
#include <stdio.h>

/* XEP-0198 §5.2 namespace version 3. */
#define XEP0198_NS "urn:xmpp:sm:3"

/* Resumption window: 300 seconds (XEP-0198 default max). */
#define XEP0198_RESUME_MAX_SECONDS 300

/* Initialise the SM module. Call from server_init() before handling connections. */
int xep0198_init(void);

/* Handle a top-level SM element (enable/r/a/resume/resumed/failed).
 * Called from on_stanza() for SM namespace elements in BOUND or ONLINE state.
 * Returns 0 on success, -1 on fatal error (caller should set pending_error). */
int xep0198_handle_element(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                           xmpp_write_fn write_fn, void* write_ud);

/* Advertise the SM stream feature. Called from send_stream_features(). */
int xep0198_append_stream_feature(xmpp_session_t* ctx);

/* ------------------------------------------------------------------ */
/*  Internal helpers (used internally by this module only)             */
/* ------------------------------------------------------------------ */

/* Append formatted XML to ctx->out_buf (session-level output buffer).
 * Mirrors iq_append() but targets the session out_buf. */
static inline int sm_write_append(xmpp_session_t* ctx, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vsnprintf(ctx->out_buf + ctx->out_len,
                     XMPP_BUF_SIZE - ctx->out_len, fmt, ap);
  va_end(ap);
  if (rc < 0) return -1;
  if ((size_t)rc >= XMPP_BUF_SIZE - ctx->out_len) return -1;
  ctx->out_len += (size_t)rc;
  return 0;
}

/* Flush ctx->out_buf to write_fn and reset the buffer.
 * Mirrors iq_flush() but targets the session out_buf. */
static inline void sm_write_flush(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* write_ud) {
  if (ctx->out_len > 0 && write_fn) {
    write_fn(write_ud, ctx->out_buf, ctx->out_len);
    ctx->out_buf[0] = '\0';
    ctx->out_len = 0;
  }
}

#endif /* XEP_0198_STREAM_MGMT_H */