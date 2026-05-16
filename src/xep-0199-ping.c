/* XEP-0199: XMPP Ping
 * https://xmpp.org/extensions/xep-0199.html
 *
 * §4.1  Server responds to client ping with an empty IQ result.
 */
#include "xep-0199-ping.h"

#include "strophe.h"
#include "xmpp_iq_buf.h"
#include "xmpp_iq_dispatch.h"

/* XEP-0199 §4.1: respond to <iq type='get'><ping/></iq> with an empty result. */
static iq_handler_result_t xep0199_handle_ping_iq(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                           xmpp_stanza_t* child, const char* iq_id) {
  (void)stanza;
  (void)child;

  char buf[256];
  size_t len = 0;
  int rc;
  if (iq_id) {
    rc = iq_append(buf, &len, sizeof(buf), "<iq type='result' id='%s'/>", iq_id);
  } else {
    rc = iq_append(buf, &len, sizeof(buf), "<iq type='result'/>");
  }
  if (rc == 0) {
    iq_flush(ctx, buf, len);
    return IQ_HANDLED;
  }
  return IQ_ERROR;
}

/* Handler registration table. */
static const iq_handler_entry_t ping_handlers[] = {
    IQ_HANDLER("urn:xmpp:ping", "get", IQ_PRIORITY_NORMAL, xep0199_handle_ping_iq),
    IQ_HANDLERS_END
};

int xep0199_init(void) {
    return iq_handler_register_all(ping_handlers);
}
