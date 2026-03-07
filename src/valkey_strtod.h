#ifndef VALKEY_STRTOD_H
#define VALKEY_STRTOD_H

#include <errno.h>
#include <stddef.h>

/**
 * Converts a string to a double using ffc.h (https://github.com/kolemannix/ffc.h),
 * a C99 port of the fast_float library.
 *
 * fast_float_strtod: takes a null-terminated string.
 * fast_float_strtod_n: takes a pointer and length, avoiding strlen.
 *
 * On success, returns the converted value and sets *endptr past the parsed characters.
 * On failure, returns 0.0, sets errno to ERANGE or EINVAL, and sets *endptr to
 * the first invalid character.
 */
double fast_float_strtod(const char *str, const char **endptr);
double fast_float_strtod_n(const char *str, size_t len, const char **endptr);

static inline double valkey_strtod(const char *str, char **endptr) {
    errno = 0;
    return fast_float_strtod(str, (const char **)endptr);
}

static inline double valkey_strtod_n(const char *str, size_t len, char **endptr) {
    errno = 0;
    return fast_float_strtod_n(str, len, (const char **)endptr);
}

#endif // VALKEY_STRTOD_H
