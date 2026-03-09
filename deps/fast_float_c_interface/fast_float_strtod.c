/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define FFC_IMPL
#include "../fast_float/ffc.h"
#include <errno.h>
#include <string.h>

static const ffc_parse_options ffc_default_options = {
    FFC_PRESET_GENERAL | FFC_FORMAT_FLAG_ALLOW_LEADING_PLUS,
    '.'
};

double fast_float_strtod(const char *str, const char **endptr) {
    double temp = 0;
    ffc_result answer = ffc_from_chars_double_options(str, str + strlen(str), &temp, ffc_default_options);
    if (answer.outcome != FFC_OUTCOME_OK) {
        errno = (answer.outcome == FFC_OUTCOME_OUT_OF_RANGE) ? ERANGE : EINVAL;
    }
    if (endptr) {
        *endptr = answer.ptr;
    }
    return temp;
}

double fast_float_strtod_n(const char *str, size_t len, const char **endptr) {
    double temp = 0;
    ffc_result answer = ffc_from_chars_double_options(str, str + len, &temp, ffc_default_options);
    if (answer.outcome != FFC_OUTCOME_OK) {
        errno = (answer.outcome == FFC_OUTCOME_OUT_OF_RANGE) ? ERANGE : EINVAL;
    }
    if (endptr) {
        *endptr = answer.ptr;
    }
    return temp;
}
