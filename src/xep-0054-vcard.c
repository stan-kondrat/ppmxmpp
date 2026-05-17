/* XEP-0054: vCard-Temp
 * https://xmpp.org/extensions/xep-0054.html
 *
 * §Use Cases:
 *   - IQ get  → fetch the vCard for a bare JID (own if no 'to')
 *   - IQ set  → store/replace own vCard; 'to' MUST be absent or own bare JID
 *   - Empty <vCard/> or service-unavailable if no vCard exists
 *
 * §Security:
 *   - Non-existent target bare JID → service-unavailable
 *   - Setting another user's vCard  → forbidden/not-allowed
 *   - server_features[] → disco#info advertises "vcard-temp"
 */
#include "xep-0054-vcard.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "storage/db.h"
#include "storage/db_users.h"
#include "storage/db_vcard.h"
#include "strophe.h"
#include "xmpp.h"
#include "xmpp_iq_buf.h"
#include "xmpp_iq_dispatch.h"

/* XEP-0054 namespace */
#define VCARD_NS "vcard-temp"

/* Extract bare JID from a full JID string (everything before the '/').
 * Returns a malloc'd copy that the caller must free(). */
static char* bare_jid_from_full(const char* full_jid) {
  if (!full_jid) return NULL;
  const char* slash = strchr(full_jid, '/');
  if (slash) {
    return strndup(full_jid, (size_t)(slash - full_jid));
  }
  return strdup(full_jid);
}

/* Send an IQ error response. */
static void send_error(xmpp_session_t* ctx, const char* iq_id,
                       const char* err_type, const char* condition) {
  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    stump_er("vCard: out of memory (error response)");
    return;
  }
  size_t len = 0;
  int rc;
  if (iq_id) {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='error' id='%s'>"
                   "<vCard xmlns='%s'/>"
                   "<error type='%s'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   iq_id, VCARD_NS, err_type, condition);
  } else {
    rc = iq_append(buf, &len, IQ_BUF_SIZE,
                   "<iq type='error'>"
                   "<vCard xmlns='%s'/>"
                   "<error type='%s'>"
                   "<%s xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                   "</error></iq>",
                   VCARD_NS, err_type, condition);
  }
  if (rc == 0) {
    iq_flush(ctx, buf, len);
  } else {
    stump_er("vCard: error stanza too large");
  }
  free(buf);
}

/* Check whether to-attribute points to a local user, a non-existent user,
 * or a foreign domain. Returns 0 on success (local + exists or no 'to'),
 * 1 for non-existent local user, 2 for foreign/unknown. */
static int resolve_target(xmpp_session_t* ctx, const char* to) {
  if (!to || !to[0]) {
    return 0;
  }

  /* Check if it's a local bare JID */
  const char* at = strchr(to, '@');
  if (at && strcmp(at + 1, ctx->domain) == 0) {
    char* bare = strndup(to, (size_t)(at - to));
    if (!bare) return 2;
    /* Check user existence */
    storage_user_t u;
    int found = storage_users_get_by_jid(to, &u);
    free(bare);
    if (found == 0) {
      return 0;  /* local user exists */
    }
    return 1;   /* local user does not exist */
  }

  return 2;  /* foreign domain */
}

/* Build a bare JID from ctx's authcid + domain. Returns malloc'd string. */
static char* own_bare_jid(const xmpp_session_t* ctx) {
  size_t cap = strlen(ctx->authcid) + 1 + strlen(ctx->domain) + 1;
  char* bare = malloc(cap);
  if (bare) {
    (void)snprintf(bare, cap, "%s@%s", ctx->authcid, ctx->domain);
  }
  return bare;
}

/* Handle IQ get for vCard. */
static iq_handler_result_t handle_vcard_get(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                             xmpp_stanza_t* child, const char* iq_id) {
  (void)child;
  const char* to = xmpp_stanza_get_attribute(stanza, "to");

  /* Classify target */
  int target = resolve_target(ctx, to);

  if (target == 1) {
    /* Non-existent local user: return service-unavailable */
    send_error(ctx, iq_id, "cancel", "service-unavailable");
    return IQ_ERROR;
  }

  if (target == 2) {
    /* Foreign domain: return item-not-found */
    send_error(ctx, iq_id, "cancel", "item-not-found");
    return IQ_ERROR;
  }

  /* Determine which JID to look up:
   * - If 'to' is present → query that JID
   * - Otherwise → query own vCard
   */
  char* lookup_jid;
  if (to && to[0]) {
    lookup_jid = bare_jid_from_full(to);
  } else {
    lookup_jid = own_bare_jid(ctx);
  }

  if (!lookup_jid) {
    send_error(ctx, iq_id, "wait", "internal-server-error");
    return IQ_ERROR;
  }

  char* vcard_xml = NULL;
  int rc = storage_vcard_get(lookup_jid, &vcard_xml);

  /* Copy to stack buffer before freeing (prevents use-after-free). */
  char lookup_buf[1024];
  (void)snprintf(lookup_buf, sizeof(lookup_buf), "%s", lookup_jid);
  free(lookup_jid);

  char* buf = (char*)malloc(IQ_BUF_SIZE);
  if (!buf) {
    if (vcard_xml) free(vcard_xml);
    stump_er("vCard get: out of memory");
    return IQ_ERROR;
  }
  size_t len = 0;

  if (rc == 0 && vcard_xml) {
    /* vCard found — return it wrapped in IQ result */
    int ap = iq_append(buf, &len, IQ_BUF_SIZE,
                       "<iq type='result' id='%s' from='%s'>"
                       "<vCard xmlns='%s'>",
                       iq_id ? iq_id : "", lookup_buf, VCARD_NS);
    if (ap != 0) {
      stump_er("vCard get: response too large");
      free(vcard_xml);
      free(buf);
      return IQ_ERROR;
    }
    /* Append the stored vCard XML content */
    size_t xml_len = strlen(vcard_xml);
    if (len + xml_len < IQ_BUF_SIZE) {
      memcpy(buf + len, vcard_xml, xml_len);
      len += xml_len;
    }
    if (iq_append(buf, &len, IQ_BUF_SIZE, "</vCard></iq>") != 0) {
      stump_er("vCard get: response too large");
      free(vcard_xml);
      free(buf);
      return IQ_ERROR;
    }
    free(vcard_xml);
    iq_flush(ctx, buf, len);
    free(buf);
    return IQ_HANDLED;
  }

  /* No vCard found — return empty vCard per XEP-0054 §"Retrieving One's vCard" */
  if (iq_append(buf, &len, IQ_BUF_SIZE,
                "<iq type='result' id='%s' from='%s'>"
                "<vCard xmlns='%s'/>"
                "</iq>",
                iq_id ? iq_id : "", to ? to : "", VCARD_NS) != 0) {
    stump_er("vCard get: response too large");
    free(buf);
    return IQ_ERROR;
  }
  iq_flush(ctx, buf, len);
  free(buf);
  return IQ_HANDLED;
}

/* Handle IQ set for vCard. */
static iq_handler_result_t handle_vcard_set(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                                             xmpp_stanza_t* child, const char* iq_id) {
  (void)child;
  const char* to = xmpp_stanza_get_attribute(stanza, "to");

  /* XEP-0054 §"Updating One's vCard":
   * If 'to' is present it MUST be the sender's own bare JID. */
  if (to && to[0]) {
    char* own = own_bare_jid(ctx);
    if (!own || strcmp(to, own) != 0) {
      free(own);
      send_error(ctx, iq_id, "auth", "forbidden");
      return IQ_ERROR;
    }
    free(own);
  }

  /* Serialize the <vCard> subtree to an XML string.
   * Extract inner XML by cloning the <vCard> element and printing it.
   */
  char* vcard_xml = NULL;
  size_t vcard_len = 0;
  if (xmpp_stanza_to_text(child, &vcard_xml, &vcard_len) != 0 || !vcard_xml) {
    send_error(ctx, iq_id, "modify", "bad-request");
    return IQ_ERROR;
  }

  char* owner_jid = own_bare_jid(ctx);
  if (!owner_jid) {
    free(vcard_xml);
    send_error(ctx, iq_id, "wait", "internal-server-error");
    return IQ_ERROR;
  }

  int rc = storage_vcard_set(owner_jid, vcard_xml);
  free(owner_jid);
  free(vcard_xml);

  if (rc != 0) {
    send_error(ctx, iq_id, "wait", "internal-server-error");
    return IQ_ERROR;
  }

  /* Return IQ result */
  char buf[256];
  size_t len = 0;
  int ap = iq_append(buf, &len, sizeof(buf), "<iq type='result' id='%s'/>",
                      iq_id ? iq_id : "");
  if (ap == 0) {
    iq_flush(ctx, buf, len);
    return IQ_HANDLED;
  }
  return IQ_ERROR;
}

/* Handler registration table. */
static const iq_handler_entry_t vcard_handlers[] = {
    IQ_HANDLER("vcard-temp", "get", IQ_PRIORITY_NORMAL, handle_vcard_get),
    IQ_HANDLER("vcard-temp", "set", IQ_PRIORITY_NORMAL, handle_vcard_set),
    IQ_HANDLERS_END
};

int xep0054_init(void) {
    return iq_handler_register_all(vcard_handlers);
}