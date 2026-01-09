# libbacktrace
A C library that may be linked into a C/C++ program to produce symbolic backtraces

Initially written by Ian Lance Taylor <iant@golang.org>.

This is version 1.0.
It is likely that this will always be version 1.0.

The libbacktrace library may be linked into a program or library and
used to produce symbolic backtraces.
Sample uses would be to print a detailed backtrace when an error
occurs or to gather detailed profiling information.

In general the functions provided by this library are async-signal-safe,
meaning that they may be safely called from a signal handler.
That said, on systems that use `dl_iterate_phdr`, such as GNU/Linux,
the first call to a libbacktrace function will call `dl_iterate_phdr`,
which is not in general async-signal-safe.  Therefore, programs
that call libbacktrace from a signal handler should ensure that they
make an initial call from outside of a signal handler.
Similar considerations apply when arranging to call libbacktrace
from within malloc; `dl_iterate_phdr` can also call malloc,
so make an initial call to a libbacktrace function outside of
malloc before trying to call libbacktrace functions within malloc.

The libbacktrace library is provided under a BSD license.
See the source files for the exact license text.

The public functions are declared and documented in the header file
backtrace.h, which should be #include'd by a user of the library.

Building libbacktrace will generate a file backtrace-supported.h,
which a user of the library may use to determine whether backtraces
will work.
See the source file backtrace-supported.h.in for the macros that it
defines.

As of July 2024, libbacktrace supports ELF, PE/COFF, Mach-O, and
XCOFF executables with DWARF debugging information.
In other words, it supports GNU/Linux, *BSD, macOS, Windows, and AIX.
The library is written to make it straightforward to add support for
other object file and debugging formats.

The library relies on the C++ unwind API defined at
https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html
This API is provided by GCC and clang.
