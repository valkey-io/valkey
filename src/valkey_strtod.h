#ifndef VALKEY_STRTOD_H
#define VALKEY_STRTOD_H

#include "sds.h"

/**
 * Converts a string to a double using ffc.h (https://github.com/kolemannix/ffc.h),
 * a C99 port of the fast_float library.
 *
 * valkey_strtod: takes a null-terminated string.
 * valkey_strtod_n: takes a pointer and length, avoiding strlen.
 *
 * On success, returns the converted value and sets *endptr past the parsed characters.
 * On failure, returns 0.0, sets errno to ERANGE or EINVAL, and sets *endptr to
 * the first invalid character.
 */

/**
 * Converts a null-terminated string to a double-precision floating-point number.
 * On success, returns the converted value and sets *endptr to point past the
 * last parsed character. On failure, returns 0.0 and sets errno appropriately.
 */
double valkey_strtod(const char *str, char **endptr);

/**
 * Converts a string of specified length to a double-precision floating-point number.
 * Unlike valkey_strtod, this function does not require the string to be null-terminated,
 * making it suitable for parsing substrings. On success, returns the converted value
 * and sets *endptr to point past the last parsed character. On failure, returns 0.0
 * and sets errno appropriately.
 */
double valkey_strtod_n(const char *str, size_t len, char **endptr);

/**
 * Converts an SDS string to a double-precision floating-point number.
 * This is a convenience wrapper around valkey_strtod_n that automatically
 * determines the string length using sdslen(). On success, returns the converted
 * value and sets *endptr to point past the last parsed character. On failure,
 * returns 0.0 and sets errno appropriately.
 */
double valkey_strtod_sds(sds str, char **endptr);

#endif // VALKEY_STRTOD_H
