#ifndef FAST_FLOAT_STRTOD_H
#define FAST_FLOAT_STRTOD_H

#ifdef USE_FAST_FLOAT

#include "errno.h"

/**
 * Converts a null-terminated byte string to a double using the fast_float library.
 *
 * This function provides a C-compatible wrapper around the fast_float library's string-to-double
 * conversion functionality. It aims to offer a faster alternative to the standard strtod function.
 *
 * str: A pointer to the null-terminated byte string to be converted.
 * eptr: On success, stores char pointer pointing to '\0' at the end of the string.
 *       On failure, stores char pointer pointing to first invalid character in the string.
 * returns: On success, the function returns the converted double value.
 *          On failure, it returns 0.0 and stores error code in errno to ERANGE or EINVAL.
 *
 * note: This function uses the fast_float library (https://github.com/fastfloat/fast_float) for
 * the actual conversion, which can be significantly faster than standard library functions.
 * Refer to "../deps/fast_float_c_interface" for more details.
 * Refer to https://github.com/fastfloat/fast_float for more information on the underlying library.
 */
double fast_float_strtod(const char *str, char **endptr);

static inline double valkey_strtod(const char *str, char **endptr) {
    errno = 0;
    return fast_float_strtod(str, endptr);
}

#else

#include <stdlib.h>

static inline double valkey_strtod(const char *str, char **endptr) {
    return strtod(str, endptr);
}

#endif

#endif // FAST_FLOAT_STRTOD_H
