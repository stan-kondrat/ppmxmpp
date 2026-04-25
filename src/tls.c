#include "tls.h"
#include "log.h"

#include "mbedtls/pk.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/entropy.h"
#include "mbedtls/x509_crt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int ensure_parent_dir(const char* filepath) {
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s", filepath);
  char* slash = strrchr(dir, '/');
  if (!slash) {
    return 0;
  }
  *slash = '\0';

  for (char* p = dir + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        stump_er("cannot create directory '%s': %s", dir, strerror(errno));
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    stump_er("cannot create directory '%s': %s", dir, strerror(errno));
    return -1;
  }
  return 0;
}

static int write_pem_file(const char* path, const char* pem_data) {
  FILE* f = fopen(path, "w");
  if (!f) {
    stump_er("cannot create '%s': %s", path, strerror(errno));
    return -1;
  }
  fputs(pem_data, f);
  fclose(f);
  return 0;
}

int generate_self_signed_cert(const char* cert_path, const char* key_path) {
  int ret;
  psa_status_t psa_ret;
  mbedtls_svc_key_id_t key_id;
  psa_key_attributes_t key_attrs = psa_key_attributes_init();
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_entropy_context entropy;
  mbedtls_x509_san_list san;
  unsigned char buf[4096];

  if (ensure_parent_dir(cert_path) != 0) {
    return -1;
  }
  if (ensure_parent_dir(key_path) != 0) {
    return -1;
  }

  psa_crypto_init();

  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_entropy_init(&entropy);

  const char* pers = "crt";

  ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers,
                              strlen(pers));
  if (ret != 0) {
    stump_er("ctr_drbg seed failed: -0x%04x", -ret);
    goto cleanup;
  }

  psa_set_key_type(&key_attrs, PSA_KEY_TYPE_RSA_KEY_PAIR);
  psa_set_key_bits(&key_attrs, 2048);
  psa_set_key_usage_flags(&key_attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
  psa_set_key_algorithm(&key_attrs, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256));
  psa_set_key_lifetime(&key_attrs, PSA_KEY_LIFETIME_VOLATILE);

  psa_ret = psa_generate_key(&key_attrs, &key_id);
  if (psa_ret != PSA_SUCCESS) {
    stump_er("psa_generate_key failed: %d", psa_ret);
    ret = -1;
    goto cleanup;
  }

  ret = mbedtls_pk_wrap_psa(&key, key_id);
  if (ret != 0) {
    stump_er("mbedtls_pk_wrap_psa failed: -0x%04x", -ret);
    psa_destroy_key(key_id);
    goto cleanup;
  }

  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);

  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

  mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost");
  mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost");

  mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20300101000000");

  unsigned char serial[1] = {1};
  mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));

  mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

  mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                MBEDTLS_X509_KU_KEY_ENCIPHERMENT);

  memset(&san, 0, sizeof(san));
  san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
  san.node.san.unstructured_name.p = (unsigned char*)"localhost";
  san.node.san.unstructured_name.len = strlen("localhost");
  san.next = NULL;

  mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san);

  memset(buf, 0, sizeof(buf));

  ret = mbedtls_x509write_crt_pem(&crt, buf, sizeof(buf));
  if (ret != 0) {
    stump_er("certificate PEM encoding failed: -0x%04x", -ret);
    goto cleanup;
  }

  if (write_pem_file(cert_path, (const char*)buf) != 0) {
    goto cleanup;
  }
  stump_i("generated TLS certificate: %s", cert_path);

  memset(buf, 0, sizeof(buf));
  ret = mbedtls_pk_write_key_pem(&key, buf, sizeof(buf));
  if (ret != 0) {
    stump_er("key PEM encoding failed: -0x%04x", -ret);
    goto cleanup;
  }

  if (write_pem_file(key_path, (const char*)buf) != 0) {
    goto cleanup;
  }
  stump_i("generated TLS key: %s", key_path);

cleanup:
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  psa_reset_key_attributes(&key_attrs);

  return (ret == 0) ? 0 : -1;
}
