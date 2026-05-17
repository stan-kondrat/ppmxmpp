/* XEP-0184: Message Delivery Receipts
 * https://xmpp.org/extensions/xep-0184.html
 *
 * §1    Client requests a receipt via <request xmlns='urn:xmpp:receipts'/>
 * §1    Recipient returns <received id='orig_id' xmlns='urn:xmpp:receipts'/>
 * §2    Optional <displayed xmlns='urn:xmpp:receipts'/>
 * §3    disco#info includes 'urn:xmpp:receipts' feature
 *
 * No new storage — pure message-layer feature.
 */
#include "xep-0184-receipts.h"

#include <stddef.h>
#include <string.h>

#include "strophe.h"
#include "log.h"
#include "xmpp_iq_buf.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Iterate children of stanza looking for a child element with name==el_name
 * and ns==XEP0184_NS.  Returns the matching child or NULL. */
static xmpp_stanza_t* find_receipt_child(const xmpp_stanza_t* stanza, const char* el_name) {
  xmpp_stanza_t* child = xmpp_stanza_get_children((xmpp_stanza_t*)stanza);
  while (child) {
    const char* name = xmpp_stanza_get_name(child);
    const char* ns = xmpp_stanza_get_ns(child);
    if (name && strcmp(name, el_name) == 0 &&
        ns && strcmp(ns, XEP0184_NS) == 0) {
      return child;
    }
    child = xmpp_stanza_get_next(child);
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int xep0184_has_request(const xmpp_stanza_t* stanza) {
  return find_receipt_child(stanza, "request") != NULL ? 1 : 0;
}

int xep0184_has_received(const xmpp_stanza_t* stanza) {
  return find_receipt_child(stanza, "received") != NULL ? 1 : 0;
}

int xep0184_has_displayed(const xmpp_stanza_t* stanza) {
  return find_receipt_child(stanza, "displayed") != NULL ? 1 : 0;
}

/* Send a delivery receipt: <message to='sender' from='recipient'>
 *   <received xmlns='urn:xmpp:receipts' id='orig_id'/>
 * </message> */
int xep0184_send_receipt(xmpp_session_t* ctx, const char* from_jid,
                          const char* to_jid, const char* orig_id) {
  if (!ctx || !from_jid || !to_jid || !orig_id) return -1;

  char buf[1024];
  size_t len = 0;
  int rc = iq_append(buf, &len, sizeof(buf),
                     "<message from='%s' to='%s'>"
                     "<received xmlns='%s' id='%s'/>"
                     "</message>",
                     from_jid, to_jid, XEP0184_NS, orig_id);
  if (rc != 0) {
    stump_er("xep0184: buffer overflow building receipt from=%s to=%s id=%s",
             from_jid, to_jid, orig_id);
    return -1;
  }

  ctx->write_fn(ctx->write_ud, buf, len);
  stump_d("xep0184: sent receipt to=%s id=%s", to_jid, orig_id);
  return 0;
}

/* Send a <displayed/> receipt: same structure but <displayed> element. */
int xep0184_send_displayed(xmpp_session_t* ctx, const char* from_jid,
                            const char* to_jid, const char* orig_id) {
  if (!ctx || !from_jid || !to_jid || !orig_id) return -1;

  char buf[1024];
  size_t len = 0;
  int rc = iq_append(buf, &len, sizeof(buf),
                     "<message from='%s' to='%s'>"
                     "<displayed xmlns='%s' id='%s'/>"
                     "</message>",
                     from_jid, to_jid, XEP0184_NS, orig_id);
  if (rc != 0) {
    stump_er("xep0184: buffer overflow building displayed from=%s to=%s id=%s",
             from_jid, to_jid, orig_id);
    return -1;
  }

  ctx->write_fn(ctx->write_ud, buf, len);
  stump_d("xep0184: sent displayed receipt to=%s id=%s", to_jid, orig_id);
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Module init                                                        */
/* ------------------------------------------------------------------ */

int xep0184_init(void) {
  /* 'urn:xmpp:receipts' is registered as a disco feature in
   * xep-0030-service-discovery.c (server_features[]). */
  stump_d("xep0184: init complete, feature registered via disco");
  return 0;
}