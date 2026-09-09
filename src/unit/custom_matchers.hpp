#ifndef _CUSTOM_MATCHERS_HPP_
#define _CUSTOM_MATCHERS_HPP_

#include "wrappers.h"
#include <string>

/* Matchers can be used for complex comparisons inside of EXPECT_THAT(val, matcher)
 * For example, to check if an robj contains a string "abc", a matcher can be used like:
 *   EXPECT_THAT(o, robjEqualsStr("abc"));
 */

// Matches an robj (which MUST contain an sds encoded string) to a char* string.
MATCHER_P(robjEqualsStr, str, "robj string matcher") {
    assert(arg->type == OBJ_STRING);
    assert(sdsEncodedObject(arg));

    if (strcmp(static_cast<const char *>(objectGetVal(arg)), str) == 0) return true;

    *result_listener << "robj(\"" << (char *)objectGetVal(arg) << "\") doesn't match \"" << str << "\"";
    return false;
}

#endif // _CUSTOM_MATCHERS_HPP_
