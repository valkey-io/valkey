/* testlib.h -- Header for test functions for libbacktrace library
   Copyright (C) 2012-2024 Free Software Foundation, Inc.
   Written by Ian Lance Taylor, Google.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    (1) Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    (2) Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

    (3) The name of the author may not be used to
    endorse or promote products derived from this software without
    specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.  */

#ifndef LIBBACKTRACE_TESTLIB_H
#define LIBBACKTRACE_TESTLIB_H

/* Portable attribute syntax.  Actually some of these tests probably
   won't work if the attributes are not recognized.  */

#ifndef GCC_VERSION
# define GCC_VERSION (__GNUC__ * 1000 + __GNUC_MINOR__)
#endif

#if (GCC_VERSION < 2007)
# define __attribute__(x)
#endif

#ifndef ATTRIBUTE_UNUSED
# define ATTRIBUTE_UNUSED __attribute__ ((__unused__))
#endif

/* Used to collect backtrace info.  */

struct info
{
  char *filename;
  int lineno;
  char *function;
};

/* Passed to backtrace callback function.  */

struct bdata
{
  struct info *all;
  size_t index;
  size_t max;
  int failed;
};

/* Passed to backtrace_simple callback function.  */

struct sdata
{
  uintptr_t *addrs;
  size_t index;
  size_t max;
  int failed;
};

/* Passed to backtrace_syminfo callback function.  */

struct symdata
{
  const char *name;
  uintptr_t val, size;
  int failed;
};

/* The backtrace state.  */

extern void *state;

/* The number of failures.  */

extern int failures;

extern const char *base (const char *p);
extern void check (const char *name, int index, const struct info *all,
		   int want_lineno, const char *want_function,
		   const char *want_file, int *failed);
extern int callback_one (void *, uintptr_t, const char *, int, const char *);
extern void error_callback_one (void *, const char *, int);
extern int callback_two (void *, uintptr_t);
extern void error_callback_two (void *, const char *, int);
extern void callback_three (void *, uintptr_t, const char *, uintptr_t,
			    uintptr_t);
extern void error_callback_three (void *, const char *, int);
extern void error_callback_create (void *, const char *, int);

#endif /* !defined(LIBBACKTRACE_TESTLIB_H) */
