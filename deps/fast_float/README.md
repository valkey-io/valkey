# FAST_FLOAT

The fast_float library provides fast header-only implementations for the C++ from_chars
functions for `float` and `double` types as well as integer types.  These functions convert ASCII strings representing decimal values (e.g., `1.3e10`) into binary types. We provide exact rounding (including
round to even). In our experience, these `fast_float` functions many times faster than comparable number-parsing functions from existing C++ standard libraries.

Specifically, `fast_float` provides the following two functions to parse floating-point numbers with a C++17-like syntax (the library itself only requires C++11):

```C++
from_chars_result from_chars(const char* first, const char* last, float& value, ...);
from_chars_result from_chars(const char* first, const char* last, double& value, ...);
```

## INTEGRATION

1. `fast_float_strtod` is a wrapper around the `from_chars` function.

## RESOURCES

1. fast_float: https://github.com/fastfloat/fast_float


...REMOVE THIS LATER...

- [x] Create the wrapper.
- [] Compile it.
- [] Replace it with strtod.
- [] Benchmark it.
- [] Run it on different platforms.

