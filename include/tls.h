#ifndef TLS_H
#define TLS_H

/* Generate a self-signed certificate and key at the given paths.
 * Creates parent directories if they do not exist.
 * Returns 0 on success, -1 on error. */
int generate_self_signed_cert(const char *cert_path, const char *key_path);
int file_exists(const char *path);

#endif /* TLS_H */
