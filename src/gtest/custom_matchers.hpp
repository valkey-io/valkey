#ifndef _CUSTOM_MATCHERS_HPP_
#define _CUSTOM_MATCHERS_HPP_

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <string>


// Use this matcher instead of standard StrEq. This matcher can handle strings passed
// as 'void*' by static cast them into 'const char*'.
MATCHER_P(valkeyStrEq, expected, "") {
    return std::string(static_cast<const char*>(arg)) == expected;
}

// Matches an robj (which MUST be a raw string) to a char* string.
MATCHER_P(robjEqualsStr, str, "robj string matcher") {
    assert(arg->type == OBJ_STRING);
    assert(sdsEncodedObject(arg));
    return strcmp(static_cast<const char*>(arg->ptr), str) == 0;
}

#endif // _CUSTOM_MATCHERS_HPP_
