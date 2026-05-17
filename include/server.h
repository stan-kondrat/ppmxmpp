#ifndef SERVER_H
#define SERVER_H

#include <uv.h>

/* Initialize TCP server, bind, listen, and install SIGINT/SIGTERM handlers.
 * Returns 0 on success, -1 on error. */
int server_start(uv_loop_t* loop);

/* Initialize all subsystems (logging, IQ dispatch, XEP handlers, database).
 * Must be called before server_start(). Returns 0 on success, -1 on error. */
int server_init(void);

/* Tear down all subsystems initialized by server_init(). */
void server_shutdown(void);

/* Free the global TLS context if it was initialized by server_start().
 * Call after draining the event loop to avoid leaking mbedTLS resources. */
void server_tls_cleanup(void);

#endif /* SERVER_H */
