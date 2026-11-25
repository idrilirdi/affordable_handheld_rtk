#include "textbuf.h"

#include <stdarg.h>
#include <stdio.h>

size_t appendf(char *out, size_t max, size_t n, const char *fmt, ...) {
  if (!max) return 0;
  if (n >= max) return max - 1;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(out + n, max - n, fmt, ap);
  va_end(ap);
  if (w < 0) return n;
  n += (size_t)w;
  return n >= max ? max - 1 : n;
}
