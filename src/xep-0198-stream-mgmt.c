/* XEP-0198: Stream Management — implementation
 * https://xmpp.org/extensions/xep-0198.html
 *
 * Scope (pure connection-layer, no storage):
 *   • SM feature advertisement in stream features.
 *   • <enable/> / <enabled/> handshake (with optional resumption).
 *   • <r/> ack-request / <a h='N'/> ack-response.
 *   • <resume/> / <resumed/> / <failed/> resumption.
 *   • SM-ID: HMAC-SHA256(authcid || stream_id) — server-side secret key.
 *
 * The counter 'h' (handled stanzas) follows RFC 6120 unsignedInt
 * semantics: it wraps from 2^32-1 back to 0.
 */
#include "xep-0198-stream-mgmt.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "strophe.h"
#include "xmpp_iq_buf.h"

/* PSA Crypto for HMAC-SHA256 SM-ID generation. */
#include "mbedtls/psa_util.h"
#include "psa/crypto.h"
#include "tf-psa-crypto/build_info.h"

/* ------------------------------------------------------------------ */
/*  SM-ID: HMAC-SHA256 over authcid || stream_id                       */
/* ------------------------------------------------------------------ */

/* Generate an opaque, server-bound SM-ID for session resumption.
 * SM-ID = HMAC-SHA256(key, authcid || ':' || stream_id)
 * The key is derived from /dev/urandom at startup and kept in memory.
 * The HMAC output is hex-encoded into a ~64-character string.
 *
 * key_data: HMAC key material (32 bytes of SHA-256-strength randomness).
 * key_len:  must be 32.
 * input_data: authcid || ':' || stream_id (concatenated C strings).
 * input_len: byte length of the concatenated input.
 * out_hex:   output buffer, must be >= (32 * 2 + 1) = 65 bytes.
 *
 * Returns 0 on success, -1 on PSA error.
 */
static int smid_generate_hmac_hex(const unsigned char* key_data, size_t key_len,
                                   const unsigned char* input_data, size_t input_len,
                                   char* out_hex, size_t hex_cap) {
  if (!key_data || !input_data || !out_hex || hex_cap < 65) {
    return -1;
  }

  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
  psa_set_key_bits(&attrs, (uint32_t)(key_len * 8U));

  mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
  psa_status_t st = psa_import_key(&attrs, key_data, key_len, &key_id);
  if (st != PSA_SUCCESS) {
    stump_er("smid: psa_import_key failed: %d", (int)st);
    return -1;
  }

  uint8_t mac[32];
  size_t mac_len = 0;
  st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                       input_data, input_len,
                       mac, sizeof(mac), &mac_len);
  psa_destroy_key(key_id);
  if (st != PSA_SUCCESS) {
    stump_er("smid: psa_mac_compute failed: %d", (int)st);
    return -1;
  }

  /* Hex-encode the 32-byte MAC. */
  static const char hex_digits[] = "0123456789abcdef";
  for (size_t i = 0; i < mac_len; i++) {
    out_hex[i * 2]     = hex_digits[mac[i] >> 4];
    out_hex[i * 2 + 1] = hex_digits[mac[i] & 0x0F];
  }
  out_hex[mac_len * 2] = '\0';

  (void)hex_cap; /* Already validated above. */
  return 0;
}

/* Server-side HMAC key for SM-ID generation. Derived once at startup. */
static unsigned char g_smid_key[32];
static int g_smid_key_ready = 0;

static int smid_load_key(void) {
  if (g_smid_key_ready) return 0;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    stump_er("smid: cannot open /dev/urandom");
    return -1;
  }
  ssize_t n = read(fd, g_smid_key, sizeof(g_smid_key));
  close(fd);
  if (n != (ssize_t)sizeof(g_smid_key)) {
    stump_er("smid: short read from /dev/urandom");
    return -1;
  }
  g_smid_key_ready = 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Counter helpers                                                    */
/* ------------------------------------------------------------------ */

/* Check if peer_h is valid: it must not exceed our outbound counter + 1.
 * Returns 0 if valid, -1 if the peer acknowledged more stanzas than sent. */
static int counter_check_peer_h(uint32_t peer_h, uint32_t our_outbound) {
  /* h=0 means no stanzas handled yet; the peer should never send h=1
   * if we have sent zero stanzas (our_outbound == 0). */
  if (peer_h > our_outbound) {
    stump_w("sm: peer h=%u exceeds our outbound=%u", peer_h, our_outbound);
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  String helpers                                                     */
/* ------------------------------------------------------------------ */

/* Parse a boolean attribute ('true'/'1' → true, anything else → false). */
static int parse_bool_attr(const char* val) {
  if (!val) return 0;
  return (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
}

/* Parse unsigned 32-bit integer from a string attribute.
 * Returns 0 on success, -1 on overflow or parse error. */
static int parse_uint32_attr(const char* val, uint32_t* out) {
  if (!val || !out) return -1;
  uint64_t acc = 0;
  for (const char* p = val; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
    acc = acc * 10 + (uint64_t)(*p - '0');
    if (acc > UINT32_MAX) return -1;
  }
  *out = (uint32_t)acc;
  return 0;
}

/* ------------------------------------------------------------------ */
/*  SM element handlers                                                */
/* ------------------------------------------------------------------ */

/* Handle <enable xmlns='urn:xmpp:sm:3' [resume='true']/> in BOUND state.
 * Per XEP-0198 §3, client MUST NOT enable before binding (enforced here).
 * Only one <enable/> is allowed per session. */
static void sm_handle_enable(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                              xmpp_write_fn write_fn, void* write_ud) {
  if (ctx->sm_enabled) {
    /* XEP-0198 §3: second enable → stream error. */
    stump_w("sm: duplicate <enable/> conn_id='%s'", ctx->conn_id);
    sm_write_append(ctx, "<stream:error>"
                       "<unexpected-request xmlns='urn:ietf:params:xml:ns:xmpp-streams'/>"
                       "</stream:error>");
    sm_write_append(ctx, "</stream:stream>");
    sm_write_flush(ctx, write_fn, write_ud);
    ctx->pending_error = 1;
    return;
  }

  ctx->sm_enabled = 1;
  ctx->sm_outbound = 0;   /* counter for stanzas we have sent */
  ctx->sm_handled  = 0;   /* counter for stanzas we have received and handled */
  ctx->sm_resumable = 0;  /* reset — will be set only on success below */

  int resume_requested = parse_bool_attr(xmpp_stanza_get_attribute(stanza, "resume"));

  /* Generate SM-ID using HMAC-SHA256 over authcid || ':' || stream_id. */
  if (resume_requested) {
    if (smid_load_key() == 0) {
      /* Build input: authcid ':' stream_id */
      size_t ac_len = strlen(ctx->authcid);
      size_t sid_len = strlen(ctx->stream_id);
      size_t in_len = ac_len + 1 + sid_len;  /* +1 for ':' */
      unsigned char* in_buf = malloc(in_len + 1);
      if (in_buf) {
        memcpy(in_buf, ctx->authcid, ac_len);
        in_buf[ac_len] = ':';
        memcpy(in_buf + ac_len + 1, ctx->stream_id, sid_len);

        char hex_out[65];
        if (smid_generate_hmac_hex(g_smid_key, sizeof(g_smid_key),
                                   in_buf, in_len, hex_out, sizeof(hex_out)) == 0) {
          /* Truncate to 48 hex chars (24 bytes) to keep the SM-ID short. */
          hex_out[48] = '\0';
          (void)snprintf(ctx->sm_id, sizeof(ctx->sm_id), "%s", hex_out);
          ctx->sm_resumable = 1;
        }
        free(in_buf);
      }
    }

    if (ctx->sm_resumable) {
      sm_write_append(ctx, "<enabled xmlns='%s' id='%s' resume='true' max='%d'/>",
                   XEP0198_NS, ctx->sm_id, XEP0198_RESUME_MAX_SECONDS);
    } else {
      /* Server won't allow resumption (key not loaded). */
      sm_write_append(ctx, "<enabled xmlns='%s'/>", XEP0198_NS);
    }
  } else {
    sm_write_append(ctx, "<enabled xmlns='%s'/>", XEP0198_NS);
  }

  sm_write_flush(ctx, write_fn, write_ud);
  stump_d("sm: enabled conn_id='%s' resumable=%d", ctx->conn_id, ctx->sm_resumable);
}

/* Handle <r xmlns='urn:xmpp:sm:3'/> ack-request in ONLINE state.
 * Respond with <a h='N'/> where N = our inbound handled counter. */
static void sm_handle_ack_req(xmpp_session_t* ctx, xmpp_write_fn write_fn, void* write_ud) {
  sm_write_append(ctx, "<a xmlns='%s' h='%u'/>", XEP0198_NS, ctx->sm_handled);
  sm_write_flush(ctx, write_fn, write_ud);
  stump_d("sm: ack-req -> h=%u", ctx->sm_handled);
}

/* Handle <a xmlns='urn:xmpp:sm:3' h='N'/> ack in ONLINE state.
 * The peer tells us they have handled stanzas up to sequence number N.
 * Mark our outbound stanzas up to N as acknowledged (advance sm_outbound). */
static void sm_handle_ack(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                         xmpp_write_fn write_fn, void* write_ud) {
  const char* h_attr = xmpp_stanza_get_attribute(stanza, "h");
  uint32_t peer_h = 0;
  if (parse_uint32_attr(h_attr, &peer_h) != 0) {
    stump_w("sm: malformed 'h' in <a/> conn_id='%s'", ctx->conn_id);
    return;
  }

  /* XEP-0198 §4.1: invalid h value → stream error. */
  if (counter_check_peer_h(peer_h, ctx->sm_outbound) != 0) {
    sm_write_append(ctx, "<stream:error>"
                       "<undefined-condition xmlns='urn:ietf:params:xml:ns:xmpp-streams'>"
                       "<handled-count-too-high xmlns='urn:xmpp:sm:3' "
                       "h='%u' send-count='%u'/>"
                       "</undefined-condition>"
                       "<text xml:lang='en' xmlns='urn:ietf:params:xml:ns:xmpp-streams'>"
                       "You acknowledged %u stanzas, but I only sent you %u so far."
                       "</text>"
                       "</stream:error>",
                   peer_h, ctx->sm_outbound, peer_h, ctx->sm_outbound);
    sm_write_append(ctx, "</stream:stream>");
    sm_write_flush(ctx, write_fn, write_ud);
    ctx->pending_error = 1;
    return;
  }

  ctx->sm_outbound = peer_h;
  stump_d("sm: ack h=%u", peer_h);
}

/* Handle <resume xmlns='urn:xmpp:sm:3' h='N' previd='SM-ID'/> in ONLINE state.
 * Per XEP-0198 §5, session resumption re-uses the existing stream state.
 * We verify the SM-ID and handle the 'h' value. */
static void sm_handle_resume(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                             xmpp_write_fn write_fn, void* write_ud) {
  const char* h_attr   = xmpp_stanza_get_attribute(stanza, "h");
  const char* previd   = xmpp_stanza_get_attribute(stanza, "previd");

  uint32_t resume_h = 0;
  if (parse_uint32_attr(h_attr, &resume_h) != 0) {
    stump_w("sm: malformed 'h' in <resume/> conn_id='%s'", ctx->conn_id);
    goto sm_failed;
  }

  /* Verify the SM-ID matches what we issued for this session.
   * If SM was not resumable, sm_id is empty and resumes always fail. */
  if (!ctx->sm_resumable || !ctx->sm_id[0]) {
    stump_w("sm: resume requested but session not resumable conn_id='%s'", ctx->conn_id);
    goto sm_failed;
  }
  if (!previd || strcmp(previd, ctx->sm_id) != 0) {
    stump_w("sm: invalid SM-ID '%s' != '%s' conn_id='%s'",
            previd ? previd : "", ctx->sm_id, ctx->conn_id);
    goto sm_failed;
  }

  /* Valid resumption: respond with <resumed h='N' previd='SM-ID'/> and
   * carry over sequence values (do NOT reset sm_handled/sm_outbound). */
  sm_write_append(ctx, "<resumed xmlns='%s' h='%u' previd='%s'/>",
               XEP0198_NS, ctx->sm_handled, ctx->sm_id);
  sm_write_flush(ctx, write_fn, write_ud);
  ctx->sm_enabled = 1;  /* re-affirm SM is active */
  stump_d("sm: resumed conn_id='%s' h=%u", ctx->conn_id, ctx->sm_handled);
  return;

sm_failed:
  sm_write_append(ctx, "<failed xmlns='%s'>"
                     "<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                     "</failed>",
               XEP0198_NS);
  sm_write_flush(ctx, write_fn, write_ud);
}

/* Handle <failed xmlns='urn:xmpp:sm:3'> in ONLINE state.
 * SM negotiation failed — log and continue without SM. */
static void sm_handle_failed(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                             xmpp_write_fn write_fn, void* write_ud) {
  (void)stanza;
  (void)write_fn;
  (void)write_ud;
  stump_w("sm: <failed/> received conn_id='%s'", ctx->conn_id);
  /* SM did not activate — reset counters so we don't track acks. */
  ctx->sm_enabled   = 0;
  ctx->sm_resumable = 0;
  ctx->sm_id[0]     = '\0';
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Parse the top-level element name and dispatch to the appropriate handler.
 * Called from xmpp.c on_stanza() when the element is in the XEP-0198 namespace.
 *
 * Valid in states:
 *   BOUND  → <enable/>, <resume/> (resume is allowed before first enable on a
 *             fresh stream that was already SASL-authenticated).
 *   ONLINE → <r/>, <a/>, <resume/>, <resumed/> (after enable), <failed/>. */
int xep0198_handle_element(xmpp_session_t* ctx, xmpp_stanza_t* stanza,
                            xmpp_write_fn write_fn, void* write_ud) {
  if (!ctx || !stanza) return -1;

  const char* ns = xmpp_stanza_get_ns(stanza);
  if (!ns || strcmp(ns, XEP0198_NS) != 0) return 0;  /* not an SM element */

  const char* name = xmpp_stanza_get_name(stanza);

  stump_d("sm: element='%s' state=%d sm_enabled=%d conn_id='%s'",
          name, ctx->state, ctx->sm_enabled, ctx->conn_id);

  if (strcmp(name, "enable") == 0) {
    /* BOUND state only. */
    if (ctx->state != XMPP_STATE_BOUND) {
      stump_w("sm: <enable/> in unexpected state conn_id='%s'", ctx->conn_id);
      return 0;
    }
    sm_handle_enable(ctx, stanza, write_fn, write_ud);
    return 0;
  }

  if (strcmp(name, "r") == 0) {
    /* Ack request: ONLINE state. */
    if (ctx->state != XMPP_STATE_ONLINE || !ctx->sm_enabled) return 0;
    sm_handle_ack_req(ctx, write_fn, write_ud);
    return 0;
  }

  if (strcmp(name, "a") == 0) {
    /* Ack response: ONLINE state. */
    if (ctx->state != XMPP_STATE_ONLINE || !ctx->sm_enabled) return 0;
    sm_handle_ack(ctx, stanza, write_fn, write_ud);
    return 0;
  }

  if (strcmp(name, "resume") == 0) {
    /* Resumption request: allowed in BOUND (before enable on new stream) or
     * ONLINE (after a failed resumption or partial session). */
    sm_handle_resume(ctx, stanza, write_fn, write_ud);
    return 0;
  }

  if (strcmp(name, "resumed") == 0) {
    /* Server confirmed resumption (should not be received by server). */
    stump_w("sm: unexpected <resumed/> received conn_id='%s'", ctx->conn_id);
    return 0;
  }

  if (strcmp(name, "failed") == 0) {
    sm_handle_failed(ctx, stanza, write_fn, write_ud);
    return 0;
  }

  stump_d("sm: unknown element '%s' — ignored", name);
  return 0;
}

/* Advertise the SM stream feature.
 * Called from xmpp.c after offering resource binding (BOUND state).
 * XEP-0198 §2: SM is offered after SASL auth + resource binding.
 * For session resumption, the client sends <resume/> on a fresh stream,
 * and we process it during BOUND state before re-enabling. */
int xep0198_append_stream_feature(xmpp_session_t* ctx) {
  /* SM is always advertised. Resumption support is per-session. */
  return sm_write_append(ctx, "<sm xmlns='%s'/>", XEP0198_NS);
}

/* Initialise the SM module. No-op for now (PSA Crypto is init'd by tls.c). */
int xep0198_init(void) {
  stump_d("sm: module initialised");
  return 0;
}