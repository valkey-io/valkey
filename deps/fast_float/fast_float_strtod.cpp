#include "fast_float.h"
#include "fast_float_strtod.h"
#include <cerrno>

/**
 * @brief Converts a null-terminated byte string to a double using the fast_float library.
 *
 * This function provides a C-compatible wrapper around the fast_float library's string-to-double
 * conversion functionality. It aims to offer a faster alternative to the standard strtod function.
 *
 * @param nptr A pointer to the null-terminated byte string to be converted.
 * @param endptr If not NULL, a pointer to a pointer to char will be stored with the address
 *               of the first invalid character in nptr. If the function returns successfully,
 *               this will point to the null terminator or any extra characters after the number.
 *
 * @return On success, returns the converted double value.
 *         On failure, returns 0.0 and sets errno to ERANGE (if result is out of range)
 *         or EINVAL (for invalid input).
 *
 * @note This function uses the fast_float library (https://github.com/fastfloat/fast_float)
 *       for the actual conversion, which can be significantly faster than standard library functions.
 *
 * @see https://github.com/fastfloat/fast_float for more information on the underlying library.
 */

extern "C"
{
    double fast_float_strtod(const char *nptr, char **endptr)
    {
        double result;
        auto answer = fast_float::from_chars(nptr, nptr + strlen(nptr), result);

        if (answer.ec == std::errc())
        {
            if (endptr)
            {
                *endptr = const_cast<char *>(answer.ptr);
            }
            return result;
        }
        else
        {
            if (endptr)
            {
                *endptr = const_cast<char *>(answer.ptr);
            }
            errno = (answer.ec == std::errc::result_out_of_range) ? ERANGE : EINVAL;
            return 0.0;
        }
    }
}
