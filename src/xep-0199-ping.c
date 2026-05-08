#include "xep-0199-ping.h"

#include "xmpp_iq_buf.h"

void xep0199_handle_ping(xmpp_session_t* ctx, const char* iq_id) {
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
  }
}
