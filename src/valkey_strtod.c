#include "valkey_strtod.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include "sds.h"

#define FFC_IMPL
#define FFC_DEBUG 0
#include "ffc.h"

const ffc_parse_options valkey_strtod_options = {
    FFC_PRESET_GENERAL | FFC_FORMAT_FLAG_ALLOW_LEADING_PLUS,
    '.'};

/**
 * Converts a null-terminated string to a double-precision floating-point number.
 * On success, returns the converted value and sets *endptr to point past the
 * last parsed character. On failure, returns 0.0 and sets errno appropriately.
 */
double valkey_strtod(const char *str, char **endptr) {
    errno = 0;
    double temp = 0.0;
    ffc_result answer = ffc_from_chars_double_options(str, str + strlen(str), &temp, valkey_strtod_options);
    if (answer.outcome != FFC_OUTCOME_OK) {
        errno = (answer.outcome == FFC_OUTCOME_OUT_OF_RANGE) ? ERANGE : EINVAL;
    }
    if (endptr) {
        *endptr = (char *)answer.ptr;
    }
    return temp;
}

/**
 * Converts a string of specified length to a double-precision floating-point number.
 * Unlike valkey_strtod, this function does not require the string to be null-terminated,
 * making it suitable for parsing substrings. On success, returns the converted value
 * and sets *endptr to point past the last parsed character. On failure, returns 0.0
 * and sets errno appropriately.
 */
double valkey_strtod_n(const char *str, size_t len, char **endptr) {
    errno = 0;
    double temp = 0.0;
    ffc_result answer = ffc_from_chars_double_options(str, str + len, &temp, valkey_strtod_options);
    if (answer.outcome != FFC_OUTCOME_OK) {
        errno = (answer.outcome == FFC_OUTCOME_OUT_OF_RANGE) ? ERANGE : EINVAL;
    }
    if (endptr) {
        *endptr = (char *)answer.ptr;
    }
    return temp;
}

/**
 * Converts an SDS string to a double-precision floating-point number.
 * This is a convenience wrapper around valkey_strtod_n that automatically
 * determines the string length using sdslen(). On success, returns the converted
 * value and sets *endptr to point past the last parsed character. On failure,
 * returns 0.0 and sets errno appropriately.
 */
double valkey_strtod_sds(sds str, char **endptr) {
    errno = 0;
    return valkey_strtod_n(str, sdslen(str), endptr);
}
