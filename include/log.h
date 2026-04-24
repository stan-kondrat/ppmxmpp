#ifndef LOG_H
#define LOG_H

#include <stumpless.h>
#include <stumpless/level/debug.h>
#include <stumpless/level/info.h>
#include <stumpless/level/warning.h>
#include <stumpless/level/err.h>
#include <stumpless/target/stream.h>

/* Open a stdout log target. Call once at startup before any stump_* usage. */
void log_init(void);

/* Close the log target and free stumpless resources. */
void log_free(void);

#endif /* LOG_H */
