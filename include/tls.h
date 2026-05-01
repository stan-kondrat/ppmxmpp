#ifndef TLS_H
#define TLS_H

#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* Generate a self-signed certificate and key at the given paths.
 * Creates parent directories if they do not exist.
 * Returns 0 on success, -1 on error. */
int generate_self_signed_cert(const char* cert_path, const char* key_path);
int file_exists(const char* path);

/* Shared server-side TLS context (one per process, shared across connections). */
typedef struct {
  mbedtls_ssl_config conf;
  mbedtls_x509_crt cert;
  mbedtls_pk_context key;
} tls_server_ctx_t;

/* Load cert/key from PEM files and configure server-side TLS defaults.
 * psa_crypto_init() must have been called before this.
 * Returns 0 on success, -1 on error. */
int tls_server_ctx_init(tls_server_ctx_t* ctx, const char* cert_path, const char* key_path);

/* Release all resources owned by ctx. */
void tls_server_ctx_free(tls_server_ctx_t* ctx);

#endif /* TLS_H */
