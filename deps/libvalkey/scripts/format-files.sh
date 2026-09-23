#!/bin/sh

find examples include src tests \
    \( -name '*.c' -or -name '*.cpp' -or -name '*.h' \) \
    -exec clang-format -i {} + ;
