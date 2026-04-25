#ifndef SERVER_H
#define SERVER_H

#include <uv.h>

/* Initialize TCP server, bind, listen, and install SIGINT/SIGTERM handlers.
 * Returns 0 on success, -1 on error. */
int server_start(uv_loop_t* loop);

#endif /* SERVER_H */
