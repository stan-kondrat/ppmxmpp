#include <stumpless/target/stream.h>
#include <stumpless/target.h>

#include "log.h"

static struct stumpless_target *log_target;

void log_init(void) {
    log_target = stumpless_open_stdout_target("ppmxmpp");
    stumpless_set_current_target(log_target);
}

void log_free(void) {
    stumpless_close_target(log_target);
    stumpless_free_all();
}
