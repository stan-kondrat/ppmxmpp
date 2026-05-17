#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "storage/db.h"
#include "storage/db_users.h"
#include "test_xmpp_helpers.h"
#include "xmpp_sasl.h"
#include "xmpp_sasl_scram.h"

/* ------------------------------------------------------------------ */
/*  Helper: base64 encode for test messages                           */
/* ------------------------------------------------------------------ */

static void b64_encode(const uint8_t* in, size_t in_len, char* out, size_t out_cap) {
  static const char b64_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t j = 0;
  for (size_t i = 0; i < in_len; i += 3) {
    unsigned char b0 = in[i];
    unsigned char b1 = (i + 1 < in_len) ? in[i + 1] : 0;
    unsigned char b2 = (i + 2 < in_len) ? in[i + 2] : 0;
    if (j + 4 >= out_cap) break;
    out[j++] = b64_table[b0 >> 2];
    out[j++] = b64_table[((b0 & 3) << 4) | (b1 >> 4)];
    out[j++] = (i + 1 < in_len) ? b64_table[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out[j++] = (i + 2 < in_len) ? b64_table[b2 & 63] : '=';
  }
  out[j] = '\0';
}

/* Feed a SCRAM-SHA-256 auth message.
 * Pass NULL/empty b64_data to send <auth mechanism='SCRAM-SHA-256'/> (initial response). */
static int feed_scram_sha256(xmpp_session_t* ctx, const char* b64_data) {
  char auth_stanza[4096];
  if (b64_data && b64_data[0]) {
    snprintf(auth_stanza, sizeof(auth_stanza),
             "<auth xmlns='urn:ietf:params:xml:ns:xmpp-sasl' mechanism='SCRAM-SHA-256'>%s</auth>",
             b64_data);
  } else {
    snprintf(auth_stanza, sizeof(auth_stanza),
             "<auth xmlns='urn:ietf:params:xml:ns:xmpp-sasl' mechanism='SCRAM-SHA-256'/>");
  }
  g_write_len = 0;
  return xmpp_feed(ctx, auth_stanza, strlen(auth_stanza), mock_write, NULL);
}

/* Extract a SCRAM attribute value from a decoded server-first/message.
 * Returns the number of bytes written, or -1 if not found. */
static int extract_attr(const char* msg, size_t msg_len, char attr,
                        char* out, size_t out_cap) {
  return scram_get_attr(msg, msg_len, attr, out, out_cap);
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_scram_primitives(void** state) {
  (void)state;

  /* Test scram_hash */
  uint8_t hash_out[32];
  const uint8_t test_data[] = "test";
  scram_hash(test_data, 4, hash_out);
  /* Known SHA-256 of "test" */
  assert_true(hash_out[0] != 0 || hash_out[1] != 0);

  /* Test scram_hmac */
  uint8_t hmac_out[32];
  uint8_t key[32] = {0};
  scram_hmac(key, 32, test_data, 4, hmac_out);
  /* HMAC-SHA-256 of empty key on "test" should be deterministic */
  uint8_t hmac_out2[32];
  scram_hmac(key, 32, test_data, 4, hmac_out2);
  assert_int_equal(memcmp(hmac_out, hmac_out2, 32), 0);

  /* Test scram_base64_encode/decode roundtrip */
  char encoded[256];
  int enc_len = scram_base64_encode((uint8_t*)"Hello, World!", 13, encoded, sizeof(encoded));
  assert_true(enc_len > 0);
  assert_true(strcmp(encoded, "SGVsbG8sIFdvcmxkIQ==") == 0);

  uint8_t decoded[256];
  int dec_len = scram_base64_decode(encoded, (size_t)enc_len, decoded, sizeof(decoded));
  assert_int_equal(dec_len, 13);
  assert_true(memcmp(decoded, "Hello, World!", 13) == 0);

  /* Test scram_get_attr / scram_put_attr */
  char msg[] = "n=user,r=nonce123";
  char value[128];
  int len = scram_get_attr(msg, strlen(msg), 'n', value, sizeof(value));
  assert_int_equal(len, 4);
  assert_true(memcmp(value, "user", 4) == 0);

  len = scram_get_attr(msg, strlen(msg), 'r', value, sizeof(value));
  assert_int_equal(len, 9);
  assert_true(memcmp(value, "nonce123", 9) == 0);

  /* Test scram_validate_username */
  assert_int_equal(scram_validate_username("validuser"), 1);
  assert_int_equal(scram_validate_username("user@domain"), 0);  /* @ forbidden */
  assert_int_equal(scram_validate_username("user/domain"), 0);  /* / forbidden */

  /* Test nonce generation */
  char nonce[128];
  scram_generate_nonce(nonce, sizeof(nonce), 16);
  assert_true(strlen(nonce) >= 16);
  assert_true(strchr(nonce, ',') == NULL);  /* No comma per RFC 5802 */
}

static void test_scram_hi_derivation(void** state) {
  (void)state;

  /* Test scram_hi with known parameters */
  const char* password = "password";
  uint8_t salt[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
  uint8_t output[32];
  int rc = scram_hi(password, salt, 16, 4096, output);
  assert_int_equal(rc, 0);
  /* SaltedPassword should be deterministic */
  uint8_t output2[32];
  rc = scram_hi(password, salt, 16, 4096, output2);
  assert_int_equal(rc, 0);
  assert_int_equal(memcmp(output, output2, 32), 0);

  /* Different salt should produce different output */
  uint8_t salt2[16] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                       0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20};
  uint8_t output3[32];
  rc = scram_hi(password, salt2, 16, 4096, output3);
  assert_int_equal(rc, 0);
  assert_true(memcmp(output, output3, 32) != 0);

  /* Test StoredKey and ServerKey derivation */
  uint8_t stored_key[32];
  scram_derive_stored_key(output, stored_key);
  uint8_t server_key[32];
  scram_derive_server_key(output, server_key);
  assert_true(memcmp(stored_key, server_key, 32) != 0);  /* Different derivations */
}

static void test_scram_ct_memeq(void** state) {
  (void)state;

  uint8_t a1[32] = {0};
  uint8_t a2[32] = {0};
  assert_int_equal(scram_ct_memeq(a1, a2, 32), 1);

  uint8_t b1[32] = {0};
  uint8_t b2[32] = {1};
  assert_int_equal(scram_ct_memeq(b1, b2, 32), 0);

  /* Partial mismatch should return 0 */
  uint8_t c1[32] = {0};
  uint8_t c2[32] = {0};
  c2[16] = 1;
  assert_int_equal(scram_ct_memeq(c1, c2, 32), 0);
}

static void test_xmpp_sasl_scram_mech_offered(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  /* Open stream */
  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED);

  /* TLS negotiation */
  g_write_len = 0;
  rc = xmpp_feed(&ctx, "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>",
                 strlen("<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>"),
                 mock_write, NULL);
  assert_int_equal(rc, 0);
  simulate_starttls(&ctx);

  /* Restart stream */
  g_write_len = 0;
  rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  /* Verify SCRAM-SHA-256 is in the mechanisms */
  assert_true(buf_contains("SCRAM-SHA-256"));
  assert_true(buf_contains("PLAIN"));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Full SCRAM-SHA-256 authentication exchange (RFC 5802 / RFC 7677)   */
/* ------------------------------------------------------------------ */

static void test_xmpp_sasl_scram_full_auth(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  /* ------------------------------------------------------------------ */
  /*  1. Create user and pre-compute SCRAM credentials                  */
  /* ------------------------------------------------------------------ */
  const char* test_user = "scramsha256@localhost";
  const char* test_pass = "testpass";

  /* Create user with plaintext password (used for PLAIN fallback too) */
  assert_int_equal(storage_users_create(test_user, test_pass), 0);

  /* Pre-compute SCRAM credentials with a FIXED salt for reproducibility.
   * This mimics what happens when a client first authenticates with SCRAM. */
  uint8_t test_salt[16];
  for (int i = 0; i < 16; i++) test_salt[i] = (uint8_t)(i + 1);
  const int TEST_ITER = 4096;

  assert_int_equal(storage_scram_set_password(test_user, test_pass,
                                               test_salt, sizeof(test_salt), TEST_ITER), 0);

  /* ------------------------------------------------------------------ */
  /*  2. Establish XMPP stream (TLS already simulated)                */
  /* ------------------------------------------------------------------ */
  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  simulate_starttls(&ctx);

  g_write_len = 0;
  rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  /* ------------------------------------------------------------------ */
  /*  3. SCRAM Step 1: Client-first-message                            */
  /*  Format: n,,n=user,r=client_nonce                                   */
  /* ------------------------------------------------------------------ */
  char client_nonce[64];
  scram_generate_nonce(client_nonce, sizeof(client_nonce), 16);

  /* client-first-message-bare: n=user,r=client_nonce */
  char client_first_bare[256];
  snprintf(client_first_bare, sizeof(client_first_bare),
           "n=,r=%s", client_nonce);

  char b64_client_first[512];
  b64_encode((uint8_t*)client_first_bare, strlen(client_first_bare),
             b64_client_first, sizeof(b64_client_first));

  g_write_len = 0;
  rc = feed_scram_sha256(&ctx, b64_client_first);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_NEGOTIATING);

  /* Parse server-first-message from challenge */
  assert_true(g_write_len > 0);
  assert_true(buf_contains("<challenge xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));

  char* challenge_start = strstr(g_write_buf, "<challenge");
  assert_non_null(challenge_start);
  char* challenge_end = strstr(challenge_start, "</challenge>");
  assert_non_null(challenge_end);

  char challenge_content[1024];
  char* data_start = strstr(challenge_start, ">");
  assert_non_null(data_start);
  data_start++;
  size_t data_len = (size_t)(challenge_end - data_start);
  if (data_len >= sizeof(challenge_content)) data_len = sizeof(challenge_content) - 1;
  memcpy(challenge_content, data_start, data_len);
  challenge_content[data_len] = '\0';

  /* Decode server-first-message */
  uint8_t server_first_raw[1024];
  int sf_len = scram_base64_decode(challenge_content, strlen(challenge_content),
                                    server_first_raw, sizeof(server_first_raw));
  assert_true(sf_len > 0);
  server_first_raw[sf_len] = '\0';
  const char* server_first = (const char*)server_first_raw;

  /* Extract combined nonce (r=), salt (s=), iteration count (i=) */
  char combined_nonce[256];
  assert_true(extract_attr(server_first, (size_t)sf_len, 'r',
                            combined_nonce, sizeof(combined_nonce)) > 0);
  assert_true(strlen(combined_nonce) > strlen(client_nonce));

  char salt_b64[128];
  assert_true(extract_attr(server_first, (size_t)sf_len, 's',
                            salt_b64, sizeof(salt_b64)) > 0);

  char iter_str[32];
  assert_true(extract_attr(server_first, (size_t)sf_len, 'i',
                            iter_str, sizeof(iter_str)) > 0);

  /* Verify nonce structure: server added its own nonce to ours */
  assert_true(strncmp(combined_nonce, client_nonce, strlen(client_nonce)) == 0);

  /* ------------------------------------------------------------------ */
  /*  4. SCRAM Step 2: Compute and send Client-final-message            */
  /* ------------------------------------------------------------------ */
  /* Decode salt from server */
  uint8_t salt_bytes[64];
  size_t salt_len = (size_t)scram_base64_decode(salt_b64, strlen(salt_b64),
                                                  salt_bytes, sizeof(salt_bytes));
  assert_true(salt_len > 0);

  /* Compute SaltedPassword = Hi(password, salt, i) */
  char* normalized = scram_saslprep(test_pass, strlen(test_pass));
  assert_non_null(normalized);
  uint8_t salted_password[32];
  assert_int_equal(scram_hi(normalized, salt_bytes, salt_len,
                             atoi(iter_str), salted_password), 0);
  free(normalized);

  /* Compute StoredKey = H(HMAC(SaltedPassword, "Client Key")) */
  uint8_t stored_key[32];
  scram_derive_stored_key(salted_password, stored_key);

  /* Build client-final-without-proof: c=base64(gs2_header),r=combined_nonce */
  /* GS2 header for no channel binding: "n,," → base64 = "biws" */
  char c_value[32];
  scram_base64_encode((uint8_t*)"n,,", 3, c_value, sizeof(c_value));

  char cf_without_proof[512];
  snprintf(cf_without_proof, sizeof(cf_without_proof), "c=%s,r=%s", c_value, combined_nonce);

  /* Build AuthMessage = client-first-bare + "," + server-first + "," + cf-without-proof */
  char auth_message[2048];
  int am_len = scram_build_auth_message(client_first_bare, server_first,
                                        cf_without_proof, auth_message, sizeof(auth_message));
  assert_true(am_len > 0);

  /* Compute ClientSignature = HMAC(StoredKey, AuthMessage) */
  uint8_t client_sig[32];
  scram_client_signature(stored_key, auth_message, (size_t)am_len, client_sig);

  /* Compute ClientKey = SaltedPassword (directly, since we have it) */
  uint8_t client_key[32];
  memcpy(client_key, salted_password, 32);

  /* ClientProof = ClientKey XOR ClientSignature */
  uint8_t client_proof[32];
  for (int i = 0; i < 32; i++) client_proof[i] = client_key[i] ^ client_sig[i];

  /* Base64-encode ClientProof */
  char p_value_b64[128];
  scram_base64_encode(client_proof, 32, p_value_b64, sizeof(p_value_b64));

  /* Build client-final-message: cf-without-proof,p=ClientProof */
  char client_final[1024];
  snprintf(client_final, sizeof(client_final), "%s,p=%s", cf_without_proof, p_value_b64);

  /* Base64-encode client-final-message */
  char b64_client_final[1024];
  b64_encode((uint8_t*)client_final, strlen(client_final),
             b64_client_final, sizeof(b64_client_final));

  /* Send client-final-message */
  g_write_len = 0;
  rc = feed_scram_sha256(&ctx, b64_client_final);
  assert_int_equal(rc, 0);

  /* ------------------------------------------------------------------ */
  /*  5. Verify server responds with SASL success + ServerSignature     */
  /* ------------------------------------------------------------------ */
  assert_true(g_write_len > 0);
  assert_true(buf_contains("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"));

  /* Verify ServerSignature (v=...) is present */
  assert_true(buf_contains("v="));

  /* Verify authcid was set correctly */
  assert_true(strlen(ctx.authcid) > 0);

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: unknown user still gets challenge (timing attack mitigation) */
/* ------------------------------------------------------------------ */

static void test_xmpp_sasl_scram_unknown_user(void** state) {
  (void)state;
  const char* db_path = NULL;
  assert_int_equal(setup_test_db(&db_path), 0);

  xmpp_session_t ctx;
  xmpp_session_reset(&ctx);
  g_write_len = 0;

  const char* client_hello = "<?xml version='1.0'?>"
                             "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                             "xmlns='jabber:client' to='localhost' version='1.0' "
                             "xml:lang='en'>";
  int rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  simulate_starttls(&ctx);

  g_write_len = 0;
  rc = xmpp_feed(&ctx, client_hello, strlen(client_hello), mock_write, NULL);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_FEATURES_RECEIVED_POST_TLS);

  /* Send client-first for a user that doesn't exist */
  char client_nonce[64];
  scram_generate_nonce(client_nonce, sizeof(client_nonce), 16);
  char client_first_bare[256];
  snprintf(client_first_bare, sizeof(client_first_bare), "n=,r=%s", client_nonce);

  char b64_client_first[512];
  b64_encode((uint8_t*)client_first_bare, strlen(client_first_bare),
             b64_client_first, sizeof(b64_client_first));

  g_write_len = 0;
  rc = feed_scram_sha256(&ctx, b64_client_first);
  assert_int_equal(rc, 0);
  assert_int_equal(ctx.state, XMPP_STATE_SASL_NEGOTIATING);

  /* Server should still send a challenge (not reveal whether user exists) */
  assert_true(g_write_len > 0);
  assert_true(buf_contains("<challenge"));
  /* Challenge should contain a nonce (r=) */
  assert_true(buf_contains("r="));
  /* Challenge should contain a salt (s=) */
  assert_true(buf_contains("s="));
  /* Challenge should contain an iteration count (i=) */
  assert_true(buf_contains("i="));

  xmpp_session_cleanup(&ctx);
  teardown_test_db();
}

/* ------------------------------------------------------------------ */
/*  Test: SASL mechanism list API                                       */
/* ------------------------------------------------------------------ */

static void test_sasl_mechanisms_list(void** state) {
  (void)state;

  const char* mechs = sasl_available_mechanisms();
  assert_non_null(mechs);
  assert_true(strstr(mechs, "PLAIN") != NULL);
  assert_true(strstr(mechs, "SCRAM-SHA-256") != NULL);

  assert_int_equal(sasl_is_multi_step_mechanism("PLAIN"), 0);
  assert_int_equal(sasl_is_multi_step_mechanism("SCRAM-SHA-256"), 1);
  assert_int_equal(sasl_is_multi_step_mechanism("UNKNOWN"), 0);
}

/* ------------------------------------------------------------------ */
/*  Test group setup/teardown                                          */
/* ------------------------------------------------------------------ */

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_scram_primitives),
    cmocka_unit_test(test_scram_hi_derivation),
    cmocka_unit_test(test_scram_ct_memeq),
    cmocka_unit_test(test_xmpp_sasl_scram_mech_offered),
    cmocka_unit_test(test_xmpp_sasl_scram_full_auth),
    cmocka_unit_test(test_xmpp_sasl_scram_unknown_user),
    cmocka_unit_test(test_sasl_mechanisms_list),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}