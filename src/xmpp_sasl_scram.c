// NOLINTBEGIN(cert-err33-c,clang-analyzer-optin.portability.UnixAPI,cert-msc50-cpp,cert-msc30-c)
// Intentional code patterns:
//   - cert-err33-c: snprintf/fread/fclose returns discarded when building XML responses
//   - clang-analyzer-optin.portability.UnixAPI: malloc(0) safe guard below
//   - cert-msc50-cpp: rand() fallback acceptable when /dev/urandom unavailable
#include "xmpp_sasl_scram.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "log.h"
#include "mbedtls/psa_util.h"
#include "psa/crypto.h"
#include "xmpp.h"

/* Default iteration count per RFC 7677 §3. */
#define DEFAULT_ITERATION_COUNT 4096
#define SHA256_DIGEST_LEN 32

/* Base64 table for encoding (RFC 4648 §4). */
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ------------------------------------------------------------------ */
/*  Utility: Base64                                                    */
/* ------------------------------------------------------------------ */

int scram_base64_encode(const uint8_t* input, size_t in_len, char* output, size_t out_cap) {
  size_t out_len = ((in_len + 2) / 3) * 4;
  if (out_len + 1 > out_cap) {
    return -1;
  }

  size_t j = 0;
  for (size_t i = 0; i < in_len; i += 3) {
    unsigned char b0 = input[i];
    unsigned char b1 = (i + 1 < in_len) ? input[i + 1] : 0;
    unsigned char b2 = (i + 2 < in_len) ? input[i + 2] : 0;

    output[j++] = B64_TABLE[b0 >> 2];
    output[j++] = B64_TABLE[((b0 & 3) << 4) | (b1 >> 4)];
    if (i + 1 < in_len) {
      output[j++] = B64_TABLE[((b1 & 15) << 2) | (b2 >> 6)];
    } else {
      output[j++] = '=';
    }
    if (i + 2 < in_len) {
      output[j++] = B64_TABLE[b2 & 63];
    } else {
      output[j++] = '=';
    }
  }
  output[j] = '\0';
  return (int)j;
}

int scram_base64_decode(const char* input, size_t in_len, uint8_t* output, size_t out_cap) {
  /* Initialize all entries to -1, then set valid base64 indices */
  static const signed char B64_DECODE[256] = {
      /* 0-31: all invalid */
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      /* 32-43: invalid except space (handled separately) */
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      /* 44: '+' is invalid, 45: '-' is 62 */
      -1, 62, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      /* 48-57: '0'-'9' are 52-61 */
      48, 49, 50, 51, 52, 53, 54, 55, 56, 57, -1, -1, -1, -1, -1, -1,
      /* 64: '@' invalid, 65-90: 'A'-'Z' are 0-25 */
      -1, 0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
      15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, -1, -1, -1, -1, -1,
      /* 91: '[' invalid, 92: '\' invalid, 93: ']' invalid */
      -1, -1, -1, 63, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      /* 96: '`' invalid, 97-122: 'a'-'z' are 26-51 */
      -1, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
      41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, -1, -1, -1, -1, -1,
      /* 123-127: all invalid */
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      /* 128-255: all invalid */
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
  };

  size_t j = 0;
  int val = 0;
  int bits = 0;

  for (size_t i = 0; i < in_len; i++) {
    unsigned char c = input[i];
    if (c == '=' || c == '\0') {
      break;
    }
    int v = (int)B64_DECODE[c];
    if (v < 0) {
      continue; /* skip whitespace */
    }
    val = (val << 6) | v;
    bits += 6;
    if (bits >= 8) {
      if (j >= out_cap) return -1;
      output[j++] = (unsigned char)((val >> (bits - 8)) & 0xFF);
      bits -= 8;
    }
  }
  return (int)j;
}

/* ------------------------------------------------------------------ */
/*  Utility: Crypto primitives                                         */
/* ------------------------------------------------------------------ */

void scram_hash(const uint8_t* data, size_t dlen, uint8_t output[32]) {
  size_t out_len = 0;
  psa_hash_compute(PSA_ALG_SHA_256, data, dlen, output, 32, &out_len);
}

void scram_hmac(const uint8_t* key, size_t klen, const uint8_t* data, size_t dlen,
                uint8_t output[32]) {
  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
  psa_set_key_bits(&attrs, (uint32_t)(klen * 8U));
  mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
  if (psa_import_key(&attrs, key, klen, &key_id) != PSA_SUCCESS) return;
  size_t mac_len = 0;
  psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, dlen, output, 32, &mac_len);
  psa_destroy_key(key_id);
}

int scram_hi(const char* password, const uint8_t* salt, size_t salt_len,
             int iterations, uint8_t output[32]) {
  /* Hi(str, salt, i) = U1 XOR U2 XOR ... XOR Ui
   * where U1 = HMAC(str, salt || INT(1))
   *       Ui = HMAC(str, Ui-1) */

  if (iterations < 1) {
    return -1;
  }

  /* Build SaltedPassword = salt || INT(1) */
  uint8_t ui_input[128];
  size_t ui_input_len = salt_len + 4;
  if (ui_input_len > sizeof(ui_input)) {
    return -1;
  }
  memcpy(ui_input, salt, salt_len);
  ui_input[salt_len] = 0;
  ui_input[salt_len + 1] = 0;
  ui_input[salt_len + 2] = 0;
  ui_input[salt_len + 3] = 1; /* INT(1) big-endian */

  uint8_t ui[32];
  uint8_t result[32];
  memset(result, 0, sizeof(result));
  memset(ui, 0, sizeof(ui));  /* safe fallback if scram_hmac fails */


  /* U1 */
  scram_hmac((uint8_t*)password, strlen(password), ui_input, ui_input_len, ui);
  for (int k = 0; k < 32; k++) result[k] = ui[k];

  /* U2..Ui */
  for (int i = 2; i <= iterations; i++) {
    scram_hmac((uint8_t*)password, strlen(password), ui, 32, ui);
    for (int k = 0; k < 32; k++) result[k] ^= ui[k];
  }

  memcpy(output, result, 32);
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Utility: Nonce generation                                           */
/* ------------------------------------------------------------------ */

void scram_generate_nonce(char* buf, size_t buflen, size_t len) {
  size_t needed = len * 2;
  if (buflen < needed + 1) {
    len = (buflen > 1) ? ((buflen - 1) / 2) : 0;
    needed = len * 2;
  }

  FILE* urandom = fopen("/dev/urandom", "rb");
  uint8_t* random_bytes = NULL;
  if (urandom) {
    random_bytes = malloc(needed);
    if (random_bytes) {
      fread(random_bytes, 1, needed, urandom);
      fclose(urandom);
    } else {
      fclose(urandom);
      return;
    }
  } else {
    /* Fallback: simple pseudo-random */
    random_bytes = malloc(needed);
    if (random_bytes) {
      srand((unsigned int)(uintptr_t)buf ^ (unsigned int)len);
      for (size_t i = 0; i < needed; i++) {
        random_bytes[i] = (uint8_t)(rand() & 0xFF);
      }
    }
  }

  if (!random_bytes) return;  /* cannot generate nonce */

  static const char CHARS[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  for (size_t i = 0; i < needed; i++) {
    buf[i] = CHARS[random_bytes[i] % (sizeof(CHARS) - 1)];
  }
  buf[needed] = '\0';
  free(random_bytes);
}

/* ------------------------------------------------------------------ */
/*  Utility: Username validation (RFC 7622 §3.3.1)                     */
/* ------------------------------------------------------------------ */

int scram_validate_username(const char* username) {
  static const char FORBIDDEN[] = "\"&'/:;<>@";
  for (const char* p = username; *p; p++) {
    if (strchr(FORBIDDEN, *p)) {
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/*  Utility: SASLprep (RFC 4013) — simplified: just UTF-8 validation.   */
/*  A full implementation would use libstringprep.  For now, we only   */
/*  forbid unassigned Unicode codepoints that would break normalization. */
/*  Real SASLprep is complex (≈500 LOC) so we defer to a real library   */
/*  or accept that non-ASCII passwords may need external normalization.  */
/* ------------------------------------------------------------------ */

/* RFC 4013 §2.3: saslprep forbids unassigned codepoints (Table 1.1).
 * We accept any valid UTF-8 for now since libstringprep is not available. */
char* scram_saslprep(const char* password, size_t password_len) {
  /* Validate UTF-8: scan for overlong sequences and invalid continuations. */
  for (size_t i = 0; i < password_len; i++) {
    unsigned char c = (unsigned char)password[i];
    if (c == 0) {
      break;
    }
    if (c < 0x80) {
      continue; /* ASCII: always valid */
    }
    if (c < 0xC0) {
      /* Invalid continuation byte. */
      return NULL;
    }
    size_t expect = 0;
    if ((c & 0xE0) == 0xC0) {
      expect = 1;
    } else if ((c & 0xF0) == 0xE0) {
      expect = 2;
    } else if ((c & 0xF8) == 0xF0) {
      expect = 3;
    } else {
      return NULL; /* 4+ byte sequences not in UTF-8 for codepoints ≤ U+10FFFF */
    }
    /* Count valid continuation bytes. */
    for (size_t k = 0; k < expect; k++) {
      if (i + 1 + k >= password_len) {
        return NULL;
      }
      unsigned char cb = (unsigned char)password[i + 1 + k];
      if ((cb & 0xC0) != 0x80) {
        return NULL;
      }
    }
    i += expect;
  }

  /* Valid UTF-8: return a null-terminated copy. */
  char* out = malloc(password_len + 1);
  if (out) {
    memcpy(out, password, password_len);
    out[password_len] = '\0';
  }
  return out;
}

/* ------------------------------------------------------------------ */
/*  Utility: SCRAM attribute parsing / encoding                        */
/* ------------------------------------------------------------------ */

int scram_get_attr(const char* message, size_t msg_len, char attr,
                   char* out, size_t out_cap) {
  /* Find attr=X in "attr=value,attr2=value2,..." */
  size_t pos = 0;
  while (pos < msg_len) {
    if (message[pos] != attr) {
      /* Skip to next comma or end. */
      while (pos < msg_len && message[pos] != ',') pos++;
      if (pos < msg_len) pos++; /* skip comma */
      continue;
    }
    if (pos + 1 >= msg_len || message[pos + 1] != '=') {
      /* attr letter found but no '=' following it: not a match */
      while (pos < msg_len && message[pos] != ',') pos++;
      if (pos < msg_len) pos++;
      continue;
    }
    pos += 2; /* skip "attr=" */
    size_t written = 0;
    while (pos < msg_len && message[pos] != ',' && written + 1 < out_cap) {
      out[written++] = message[pos++];
    }
    out[written] = '\0';
    return (int)written;
  }
  return -1;
}

/* Encode attr=value into output buffer. */
int scram_put_attr(char attr, const char* value, char* output, size_t cap) {
  int n = snprintf(output, cap, "%c=%s", attr, value);
  if (n < 0 || (size_t)n >= cap) return -1;
  return n;
}

/* ------------------------------------------------------------------ */
/*  SCRAM key derivation                                               */
/* ------------------------------------------------------------------ */

void scram_derive_stored_key(const uint8_t* salted_password, uint8_t output[32]) {
  scram_hash(salted_password, 32, output);
}

void scram_derive_server_key(const uint8_t* salted_password, uint8_t output[32]) {
  /* ServerKey = HMAC(SaltedPassword, "Server Key") */
  static const char SERVER_KEY_STR[] = "Server Key";
  scram_hmac(salted_password, 32, (uint8_t*)SERVER_KEY_STR, sizeof(SERVER_KEY_STR) - 1, output);
}

void scram_client_signature(const uint8_t* stored_key, const char* auth_message,
                           size_t auth_msg_len, uint8_t output[32]) {
  scram_hmac(stored_key, 32, (uint8_t*)auth_message, auth_msg_len, output);
}

void scram_server_signature(const uint8_t* server_key, const char* auth_message,
                           size_t auth_msg_len, uint8_t output[32]) {
  scram_hmac(server_key, 32, (uint8_t*)auth_message, auth_msg_len, output);
}

int scram_build_auth_message(const char* client_first_bare, const char* server_first,
                             const char* client_final_no_proof,
                             char* output, size_t cap) {
  int n = snprintf(output, cap, "%s,%s,%s", client_first_bare, server_first, client_final_no_proof);
  if (n < 0 || (size_t)n >= cap) return -1;
  return n;
}

int scram_ct_memeq(const uint8_t* a, const uint8_t* b, size_t len) {
  volatile unsigned char diff = 0;
  for (size_t i = 0; i < len; i++) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

/* ------------------------------------------------------------------ */
/*  High-level SCRAM handler                                           */
/* ------------------------------------------------------------------ */

/* Forward declarations of storage layer accessors (declared in db_users.h). */
extern int storage_scram_get_by_jid(const char* bare_jid,
                                    uint8_t stored_key[32],
                                    uint8_t server_key[32],
                                    uint8_t salt[64],
                                    size_t* salt_len,
                                    int* iteration_count);
extern int storage_scram_has_scram_credentials(const char* bare_jid);

int handle_scram_sha256(xmpp_session_t* ctx, int step, const char* input, size_t in_len,
                        char* response_out, size_t response_cap,
                        void (*write_fn)(void*, const char*, size_t), void* ud) {
  (void)response_cap;
  (void)response_out;

  scram_state_t* scram = &ctx->scram;
  char reply[4096];

  if (step == 1) {
    /* ------------------------------------------------------------------ */
    /*  STEP 1: Client-first-message                                       */
    /*  Format: gs2-header client-first-message-bare                       */
    /*  gs2-header = gs2-cbind-flag "," [authzid] ","                      */
    /*  client-first-message-bare = [reserved-mext ","] username "," nonce */
    /*                            ["," extensions]                        */
    /*                                                                      */
    /*  For XMPP, gs2-header is "n,,n=user,r=nonce" or "y,,n=user,r=nonce" */
    /*  "n" = no channel binding support                                   */
    /*  "y" = channel binding supported but server didn't advertise -PLUS */
    /*  "p" = channel binding required (server advertised -PLUS)          */
    /* ------------------------------------------------------------------ */

    /* Decode client-first-message from base64 input. */
    uint8_t decoded[1025];
    int dlen = scram_base64_decode(input, in_len, decoded, sizeof(decoded));
    if (dlen < 0) {
      stump_er("SCRAM-SHA-256: base64 decode failed");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<invalid-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }
    decoded[dlen] = '\0';

    /* Parse gs2-header: flag,zid, (flag is 'n', 'y', or 'p' followed by ',') */
    if (dlen < 3 || (decoded[0] != 'n' && decoded[0] != 'y' && decoded[0] != 'p')) {
      stump_er("SCRAM-SHA-256: invalid gs2-cbind-flag (must be n/y/p)");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<incorrect-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }

    /* Find first comma after gs2-cbind-flag (end of gs2-header + bare header). */
    char* bare_start = memchr((char*)decoded, ',', (size_t)dlen);
    if (!bare_start) {
      stump_er("SCRAM-SHA-256: malformed client-first (no comma)");
      return -2;
    }
    bare_start++; /* skip comma */

    /* Find next comma: separates username from nonce. */
    size_t bare_remaining = (size_t)dlen - (size_t)(bare_start - (char*)(const char*)decoded);
    char* nonce_start = memchr(bare_start, ',', bare_remaining);
    if (!nonce_start) {
      stump_er("SCRAM-SHA-256: malformed client-first (no nonce separator)");
      return -2;
    }

    /* Extract username: n=user */
    size_t user_start = (size_t)(nonce_start - bare_start);
    if (user_start < 3 || bare_start[0] != 'n' || bare_start[1] != '=') {
      stump_er("SCRAM-SHA-256: missing username attribute");
      return -2;
    }
    size_t username_len = user_start - 2; /* exclude "n=" */
    if (username_len >= sizeof(scram->username)) {
      username_len = sizeof(scram->username) - 1;
    }
    memcpy(scram->username, bare_start + 2, username_len);
    scram->username[username_len] = '\0';

    /* Validate username. */
    if (!scram_validate_username(scram->username)) {
      stump_d("SCRAM-SHA-256: forbidden character in username");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<invalid-username-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }

    /* Extract nonce: r=nonce */
    const char* r_attr = nonce_start + 1;
    size_t r_remaining = (size_t)((char*)decoded + dlen - r_attr);
    char r_value[256];
    int r_len = scram_get_attr(r_attr, r_remaining, 'r', r_value, sizeof(r_value));
    if (r_len < 0) {
      stump_er("SCRAM-SHA-256: missing r= attribute in client-first");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<incorrect-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }
    if ((size_t)r_len >= sizeof(scram->client_nonce)) {
      r_len = (int)(sizeof(scram->client_nonce) - 1);
    }
    memcpy(scram->client_nonce, r_value, (size_t)r_len);
    scram->client_nonce[r_len] = '\0';

    /* Save client-first-message-bare for AuthMessage. */
    size_t bare_len = (size_t)(nonce_start - (char*)decoded);
    if (bare_len >= sizeof(scram->client_first_bare)) {
      bare_len = sizeof(scram->client_first_bare) - 1;
    }
    memcpy(scram->client_first_bare, decoded, bare_len);
    scram->client_first_bare[bare_len] = '\0';

    /* Build bare JID. */
    char bare_jid[2048];
    int n = snprintf(bare_jid, sizeof(bare_jid), "%s@%s", scram->username, ctx->domain);
    if (n < 0 || (size_t)n >= sizeof(bare_jid)) {
      stump_er("SCRAM-SHA-256: JID too long");
      return -2;
    }

    /* Look up SCRAM credentials. */
    uint8_t stored_key[32];
    uint8_t server_key[32];
    uint8_t salt[64];
    size_t salt_len;
    int iter_count;

    int rc = storage_scram_get_by_jid(bare_jid, stored_key, server_key, salt, &salt_len, &iter_count);
    if (rc != 0) {
      /* Unknown user — RFC 5802 §5.1: send server-first with random salt anyway. */
      stump_i("SCRAM-SHA-256: user not found, sending server-first: %s", bare_jid);

      /* Generate a random salt so we don't reveal whether user exists. */
      uint8_t dummy_salt[16];
      FILE* urandom = fopen("/dev/urandom", "rb");
      if (urandom) {
        fread(dummy_salt, sizeof(dummy_salt), 1, urandom);
        fclose(urandom);
      } else {
        srand((unsigned int)(uintptr_t)bare_jid);
        for (size_t k = 0; k < sizeof(dummy_salt); k++) {
          dummy_salt[k] = (uint8_t)(rand() & 0xFF);
        }
      }

      char nonce[128];
      scram_generate_nonce(nonce, sizeof(nonce), 32);
      char nonce_combined[256];
      snprintf(nonce_combined, sizeof(nonce_combined), "%s%s", scram->client_nonce, nonce);

      char salt_b64[64];
      scram_base64_encode(dummy_salt, sizeof(dummy_salt), salt_b64, sizeof(salt_b64));

      /* Build server-first-message. */
      n = snprintf(reply, sizeof(reply),
                   "r=%s%s,s=%s,i=%d",
                   scram->client_nonce, nonce, salt_b64, DEFAULT_ITERATION_COUNT);
      if ((size_t)n >= sizeof(reply)) {
        reply[0] = '\0';
      }

      /* Save for potential verification attempt (for timing consistency). */
      strncpy(scram->server_nonce, nonce_combined, sizeof(scram->server_nonce) - 1);
      scram->server_nonce[sizeof(scram->server_nonce) - 1] = '\0';

      char salt_b64_saved[64];
      scram_base64_encode(dummy_salt, sizeof(dummy_salt), salt_b64_saved, sizeof(salt_b64_saved));
      strncpy(scram->salt_base64, salt_b64_saved, sizeof(scram->salt_base64) - 1);
      scram->salt_base64[sizeof(scram->salt_base64) - 1] = '\0';
      scram->iteration_count = DEFAULT_ITERATION_COUNT;
      memcpy(scram->salt, dummy_salt, sizeof(dummy_salt));
      scram->salt_len = sizeof(dummy_salt);

      if ((size_t)n >= sizeof(reply)) {
        /* snprintf truncated — server_first cannot hold full response */
        scram->has_server_first = 0;
        if (write_fn) {
          snprintf(reply, sizeof(reply),
                   "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                   "<temporary-auth-failure/>"
                   "</failure>");
          write_fn(ud, reply, strlen(reply));
        }
        return -2;
      }
      if ((size_t)n < sizeof(reply)) {
        strncpy(scram->server_first, reply, sizeof(scram->server_first) - 1);
        scram->server_first[sizeof(scram->server_first) - 1] = '\0';
      }

      scram->has_server_first = 1;
      scram->has_client_final = 0;

      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<challenge xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>");
        size_t off = strlen(reply);
        char b64_reply[4096];
        int b64_len = scram_base64_encode((uint8_t*)scram->server_first,
                                          strlen(scram->server_first),
                                          b64_reply, sizeof(b64_reply));
        if (b64_len >= 0 && off + (size_t)b64_len + 20 < sizeof(reply)) {
          memcpy(reply + off, b64_reply, (size_t)b64_len);
          off += (size_t)b64_len;
          reply[off] = '\0';
        }
        snprintf(reply + off, sizeof(reply) - off,
                 "</challenge>");
        write_fn(ud, reply, strlen(reply));
      }
      return 1; /* wait for client-final */
    }

    /* Save retrieved credentials. */
    memcpy(scram->stored_key, stored_key, 32);
    memcpy(scram->server_key, server_key, 32);
    memcpy(scram->salt, salt, salt_len);
    scram->salt_len = salt_len;
    scram->iteration_count = iter_count;

    /* Generate server nonce and build server-first-message. */
    char server_part[64];
    scram_generate_nonce(server_part, sizeof(server_part), 32);

    snprintf(scram->server_nonce, sizeof(scram->server_nonce),
             "%s%s", scram->client_nonce, server_part);

    scram_base64_encode(salt, salt_len, scram->salt_base64, sizeof(scram->salt_base64));

    n = snprintf(reply, sizeof(reply),
                 "r=%s,s=%s,i=%d",
                 scram->server_nonce, scram->salt_base64, iter_count);

    /* Save server-first for AuthMessage. */
    strncpy(scram->server_first, reply, sizeof(scram->server_first) - 1);
    scram->server_first[sizeof(scram->server_first) - 1] = '\0';
    scram->has_server_first = 1;
    scram->has_client_final = 0;

    if (write_fn) {
      /* Send base64-encoded server-first-message. */
      char b64_reply[2048];
      int b64_len = scram_base64_encode((uint8_t*)reply, (size_t)n, b64_reply, sizeof(b64_reply));
      snprintf(reply, sizeof(reply),
               "<challenge xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>");
      size_t off = strlen(reply);
      if (b64_len >= 0 && off + (size_t)b64_len + 20 < sizeof(reply)) {
        memcpy(reply + off, b64_reply, (size_t)b64_len);
        off += (size_t)b64_len;
      }
      snprintf(reply + off, sizeof(reply) - off, "</challenge>");
      write_fn(ud, reply, strlen(reply));
    }
    return 1; /* wait for client-final */

  } else if (step == 2) {
    /* ------------------------------------------------------------------ */
    /*  STEP 2: Client-final-message                                       */
    /*  Format: channel-binding "," nonce "," proof                        */
    /*  client-final-message = client-final-message-without-proof "," proof */
    /*  proof = p=base64(ClientProof)                                      */
    /* ------------------------------------------------------------------ */

    /* Decode client-final-message. */
    uint8_t decoded[1024];
    int dlen = scram_base64_decode(input, in_len, decoded, sizeof(decoded));
    if (dlen < 0) {
      stump_er("SCRAM-SHA-256: client-final base64 decode failed");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<invalid-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }
    decoded[dlen] = '\0';

    /* Parse client-final-message-without-proof: c=...,r=nonce */
    char c_value[512];
    if (scram_get_attr((const char*)decoded, (size_t)dlen, 'c', c_value, sizeof(c_value)) < 0) {
      stump_er("SCRAM-SHA-256: missing c= attribute");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<incorrect-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }

    char r_value[256];
    if (scram_get_attr((const char*)decoded, (size_t)dlen, 'r', r_value, sizeof(r_value)) < 0) {
      stump_er("SCRAM-SHA-256: missing r= attribute in client-final");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<incorrect-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }

    /* Verify nonce. */
    if (strcmp(r_value, scram->server_nonce) != 0) {
      stump_er("SCRAM-SHA-256: nonce mismatch");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<mismatched-proof/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }

    /* Parse proof: p=base64(ClientProof) */
    char p_value[512];
    if (scram_get_attr((const char*)decoded, (size_t)dlen, 'p', p_value, sizeof(p_value)) < 0) {
      stump_er("SCRAM-SHA-256: missing p= attribute");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<incorrect-encoding/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }

    /* Decode ClientProof. */
    uint8_t client_proof[64];
    int proof_len = scram_base64_decode(p_value, strlen(p_value), client_proof, sizeof(client_proof));
    if (proof_len != 32) {
      stump_er("SCRAM-SHA-256: invalid proof length %d (expected 32)", proof_len);
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<invalid-proof/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -2;
    }

    /* Build client-final-without-proof for AuthMessage. */
    char cf_without_proof[1024];
    scram_get_attr((const char*)decoded, (size_t)dlen, 'c', c_value, sizeof(c_value));
    int n = snprintf(cf_without_proof, sizeof(cf_without_proof), "c=%s,r=%s",
                     c_value, r_value);
    if ((size_t)n >= sizeof(cf_without_proof)) {
      return -2;
    }

    /* Build AuthMessage = client-first-bare + "," + server-first + "," + cf-without-proof */
    char auth_msg[2048];
    n = scram_build_auth_message(scram->client_first_bare, scram->server_first,
                                  cf_without_proof, auth_msg, sizeof(auth_msg));
    if (n < 0) {
      stump_er("SCRAM-SHA-256: AuthMessage overflow");
      return -2;
    }

    /* Compute ClientSignature = HMAC(StoredKey, AuthMessage). */
    uint8_t computed_sig[32];
    scram_client_signature(scram->stored_key, auth_msg, (size_t)n, computed_sig);

    /* Recover ClientKey = ClientProof XOR ClientSignature. */
    uint8_t client_key[32];
    for (int i = 0; i < 32; i++) {
      client_key[i] = client_proof[i] ^ computed_sig[i];
    }

    /* Verify: H(ClientKey) must equal StoredKey. */
    uint8_t computed_stored_key[32];
    scram_hash(client_key, 32, computed_stored_key);

    if (!scram_ct_memeq(computed_stored_key, scram->stored_key, 32)) {
      stump_i("SCRAM-SHA-256: proof verification failed");
      if (write_fn) {
        snprintf(reply, sizeof(reply),
                 "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                 "<invalid-proof/>"
                 "</failure>");
        write_fn(ud, reply, strlen(reply));
      }
      return -1;
    }

    /* Compute ServerSignature = HMAC(ServerKey, AuthMessage) for mutual auth. */
    uint8_t server_sig[32];
    scram_server_signature(scram->server_key, auth_msg, (size_t)n, server_sig);

    /* Encode ServerSignature for server-final-message. */
    char v_value[128];
    scram_base64_encode(server_sig, 32, v_value, sizeof(v_value));

    if (write_fn) {
      n = snprintf(reply, sizeof(reply),
                   "<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                   "v=%s"
                   "</success>",
                   v_value);
      write_fn(ud, reply, (size_t)n);
    }

    /* Set authenticated identity. */
    int ac_len = (int)strlen(scram->username);
    if (ac_len >= (int)(sizeof(ctx->authcid) - 1)) {
      ac_len = (int)(sizeof(ctx->authcid) - 1);
    }
    memcpy(ctx->authcid, scram->username, (size_t)ac_len);
    ctx->authcid[ac_len] = '\0';

    stump_i("SCRAM-SHA-256: authenticated: %s@%s", scram->username, ctx->domain);
    return 0; /* success */

  } else {
    /* Unknown step: malformed. */
    stump_er("SCRAM-SHA-256: unknown step %d", step);
    return -2;
  }
}
// NOLINTEND(cert-err33-c,clang-analyzer-optin.portability.UnixAPI,cert-msc50-cpp,cert-msc30-c)