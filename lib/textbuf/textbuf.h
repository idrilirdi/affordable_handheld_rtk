#pragma once

#include <stddef.h>

// Append to a fixed buffer without ever walking off the end.
//
// snprintf returns what it *would* have written, so accumulating its return
// value raw lets one truncated line push the offset past the buffer and turn
// the next `max - n` into an enormous size_t. Clamped once, here, rather than
// at every call site - which is where it gets forgotten.
size_t appendf(char *out, size_t max, size_t n, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
