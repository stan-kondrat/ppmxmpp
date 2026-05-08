#ifndef XMPP_IQ_BUF_H
#define XMPP_IQ_BUF_H

#include <stdarg.h>
#include <stdio.h>

#include "xmpp.h"

#define IQ_BUF_SIZE 65536

static inline int iq_append(char* buf, size_t* len, size_t cap, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
static inline int iq_append(char* buf, size_t* len, size_t cap, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap - *len) {
    return -1;
  }
  *len += (size_t)n;
  return 0;
}

static inline int iq_flush(xmpp_session_t* ctx, const char* buf, size_t len) {
  return ctx->write_fn(ctx->write_ud, buf, len);
}

#endif /* XMPP_IQ_BUF_H */
