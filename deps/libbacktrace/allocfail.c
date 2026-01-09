/* allocfail.c -- Test for libbacktrace library
   Copyright (C) 2018-2024 Free Software Foundation, Inc.

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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filenames.h"

#include "backtrace.h"
#include "backtrace-supported.h"

#include "testlib.h"

extern uint64_t get_nr_allocs (void);
extern void set_fail_at_alloc (uint64_t);
extern int at_fail_alloc_p (void);

static int test1 (void) __attribute__ ((noinline, unused));
static int f2 (int) __attribute__ ((noinline));
static int f3 (int, int) __attribute__ ((noinline));

static unsigned callback_errors = 0;

static void
error_callback_full (void *vdata ATTRIBUTE_UNUSED,
		     const char *msg ATTRIBUTE_UNUSED,
		     int errnum ATTRIBUTE_UNUSED)
{
  if (at_fail_alloc_p ())
    {
      set_fail_at_alloc (0);
      return;
    }

  callback_errors++;
}

static int
callback_full (void *vdata ATTRIBUTE_UNUSED, uintptr_t pc ATTRIBUTE_UNUSED,
	      const char *filename ATTRIBUTE_UNUSED,
	      int lineno ATTRIBUTE_UNUSED,
	      const char *function ATTRIBUTE_UNUSED)
{

  return 0;
}

static int
test1 (void)
{
  return f2 (__LINE__) + 1;
}

static int
f2 (int f1line)
{
  return f3 (f1line, __LINE__) + 2;
}

static int
f3 (int f1line ATTRIBUTE_UNUSED, int f2line ATTRIBUTE_UNUSED)
{
  int i;

  i = backtrace_full (state, 0, callback_full, error_callback_full, NULL);

  if (i != 0)
    {
      fprintf (stderr, "test1: unexpected return value %d\n", i);
      ++failures;
    }

  if (callback_errors)
      ++failures;

  return failures;
}

/* Run all the tests.  */

int
main (int argc, char **argv)
{
  uint64_t fail_at = 0;

  if (argc == 2)
    {
      fail_at = atoi (argv[1]);
      set_fail_at_alloc (fail_at);
    }

  state = backtrace_create_state (argv[0], BACKTRACE_SUPPORTS_THREADS,
				  error_callback_full, NULL);
  if (state == NULL)
    exit (failures ? EXIT_FAILURE : EXIT_SUCCESS);

#if BACKTRACE_SUPPORTED
  test1 ();
#endif

  if (argc == 1)
    fprintf (stderr, "%llu\n", (long long unsigned) get_nr_allocs ());

  exit (failures ? EXIT_FAILURE : EXIT_SUCCESS);
}
