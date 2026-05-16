#include <stumpless/severity.h>
#include <stumpless/target.h>
#include <stumpless/target/stream.h>

#include "log.h"

static struct stumpless_target* log_target;

void log_init(void) {
  if (log_target) return;
  log_target = stumpless_open_stdout_target("ppmxmpp");
  stumpless_set_current_target(log_target);
}

void log_silence(void) {
  if (!log_target) return;
  stumpless_set_target_mask(log_target, STUMPLESS_SEVERITY_MASK_UPTO(STUMPLESS_SEVERITY_WARNING));
}

void log_free(void) {
  if (!log_target) return;
  stumpless_close_target(log_target);
  log_target = NULL;
  stumpless_free_all();
}
