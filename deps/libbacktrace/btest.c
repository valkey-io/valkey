/* btest.c -- Test for libbacktrace library
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

/* This program tests the externally visible interfaces of the
   libbacktrace library.  */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "filenames.h"

#include "backtrace.h"
#include "backtrace-supported.h"

#include "testlib.h"

/* Test the backtrace function with non-inlined functions.  */

static int test1 (void) __attribute__ ((noinline, noclone, optnone, unused));
static int f2 (int) __attribute__ ((noinline, noclone));
static int f3 (int, int) __attribute__ ((noinline, noclone));

static int
test1 (void)
{
  /* Returning a value here and elsewhere avoids a tailcall which
     would mess up the backtrace.  */
  return f2 (__LINE__) + 1;
}

static int
f2 (int f1line)
{
  return f3 (f1line, __LINE__) + 2;
}

static int
f3 (int f1line, int f2line)
{
  struct info all[20];
  struct bdata data;
  int f3line;
  int i;

  data.all = &all[0];
  data.index = 0;
  data.max = 20;
  data.failed = 0;

  f3line = __LINE__ + 1;
  i = backtrace_full (state, 0, callback_one, error_callback_one, &data);

  if (i != 0)
    {
      fprintf (stderr, "test1: unexpected return value %d\n", i);
      data.failed = 1;
    }

  if (data.index < 3)
    {
      fprintf (stderr,
	       "test1: not enough frames; got %zu, expected at least 3\n",
	       data.index);
      data.failed = 1;
    }

  check ("test1", 0, all, f3line, "f3", "btest.c", &data.failed);
  check ("test1", 1, all, f2line, "f2", "btest.c", &data.failed);
  check ("test1", 2, all, f1line, "test1", "btest.c", &data.failed);

  printf ("%s: backtrace_full noinline\n", data.failed ? "FAIL" : "PASS");

  if (data.failed)
    ++failures;

  return failures;
}

/* Test the backtrace function with inlined functions.  */

static inline int test2 (void) __attribute__ ((always_inline, unused));
static inline int f12 (int) __attribute__ ((always_inline));
static inline int f13 (int, int) __attribute__ ((always_inline));

static inline int
test2 (void)
{
  return f12 (__LINE__) + 1;
}

static inline int
f12 (int f1line)
{
  return f13 (f1line, __LINE__) + 2;
}

static inline int
f13 (int f1line, int f2line)
{
  struct info all[20];
  struct bdata data;
  int f3line;
  int i;

  data.all = &all[0];
  data.index = 0;
  data.max = 20;
  data.failed = 0;

  f3line = __LINE__ + 1;
  i = backtrace_full (state, 0, callback_one, error_callback_one, &data);

  if (i != 0)
    {
      fprintf (stderr, "test2: unexpected return value %d\n", i);
      data.failed = 1;
    }

  check ("test2", 0, all, f3line, "f13", "btest.c", &data.failed);
  check ("test2", 1, all, f2line, "f12", "btest.c", &data.failed);
  check ("test2", 2, all, f1line, "test2", "btest.c", &data.failed);

  printf ("%s: backtrace_full inline\n", data.failed ? "FAIL" : "PASS");

  if (data.failed)
    ++failures;

  return failures;
}

/* Test the backtrace_simple function with non-inlined functions.  */

static int test3 (void) __attribute__ ((noinline, noclone, optnone, unused));
static int f22 (int) __attribute__ ((noinline, noclone));
static int f23 (int, int) __attribute__ ((noinline, noclone));

static int
test3 (void)
{
  return f22 (__LINE__) + 1;
}

static int
f22 (int f1line)
{
  return f23 (f1line, __LINE__) + 2;
}

static int
f23 (int f1line, int f2line)
{
  uintptr_t addrs[20];
  struct sdata data;
  int f3line;
  int i;

  data.addrs = &addrs[0];
  data.index = 0;
  data.max = 20;
  data.failed = 0;

  f3line = __LINE__ + 1;
  i = backtrace_simple (state, 0, callback_two, error_callback_two, &data);

  if (i != 0)
    {
      fprintf (stderr, "test3: unexpected return value %d\n", i);
      data.failed = 1;
    }

  if (!data.failed)
    {
      struct info all[20];
      struct bdata bdata;
      int j;

      bdata.all = &all[0];
      bdata.index = 0;
      bdata.max = 20;
      bdata.failed = 0;

      for (j = 0; j < 3; ++j)
	{
	  i = backtrace_pcinfo (state, addrs[j], callback_one,
				error_callback_one, &bdata);
	  if (i != 0)
	    {
	      fprintf (stderr,
		       ("test3: unexpected return value "
			"from backtrace_pcinfo %d\n"),
		       i);
	      bdata.failed = 1;
	    }
	  if (!bdata.failed && bdata.index != (size_t) (j + 1))
	    {
	      fprintf (stderr,
		       ("wrong number of calls from backtrace_pcinfo "
			"got %u expected %d\n"),
		       (unsigned int) bdata.index, j + 1);
	      bdata.failed = 1;
	    }
	}

      check ("test3", 0, all, f3line, "f23", "btest.c", &bdata.failed);
      check ("test3", 1, all, f2line, "f22", "btest.c", &bdata.failed);
      check ("test3", 2, all, f1line, "test3", "btest.c", &bdata.failed);

      if (bdata.failed)
	data.failed = 1;

      for (j = 0; j < 3; ++j)
	{
	  struct symdata symdata;

	  symdata.name = NULL;
	  symdata.val = 0;
	  symdata.size = 0;
	  symdata.failed = 0;

	  i = backtrace_syminfo (state, addrs[j], callback_three,
				 error_callback_three, &symdata);
	  if (i == 0)
	    {
	      fprintf (stderr,
		       ("test3: [%d]: unexpected return value "
			"from backtrace_syminfo %d\n"),
		       j, i);
	      symdata.failed = 1;
	    }

	  if (!symdata.failed)
	    {
	      const char *expected;

	      switch (j)
		{
		case 0:
		  expected = "f23";
		  break;
		case 1:
		  expected = "f22";
		  break;
		case 2:
		  expected = "test3";
		  break;
		default:
		  assert (0);
		}

	      if (symdata.name == NULL)
		{
		  fprintf (stderr, "test3: [%d]: NULL syminfo name\n", j);
		  symdata.failed = 1;
		}
	      /* Use strncmp, not strcmp, because GCC might create a
		 clone.  */
	      else if (strncmp (symdata.name, expected, strlen (expected))
		       != 0)
		{
		  fprintf (stderr,
			   ("test3: [%d]: unexpected syminfo name "
			    "got %s expected %s\n"),
			   j, symdata.name, expected);
		  symdata.failed = 1;
		}
	    }

	  if (symdata.failed)
	    data.failed = 1;
	}
    }

  printf ("%s: backtrace_simple noinline\n", data.failed ? "FAIL" : "PASS");

  if (data.failed)
    ++failures;

  return failures;
}

/* Test the backtrace_simple function with inlined functions.  */

static inline int test4 (void) __attribute__ ((always_inline, unused));
static inline int f32 (int) __attribute__ ((always_inline));
static inline int f33 (int, int) __attribute__ ((always_inline));

static inline int
test4 (void)
{
  return f32 (__LINE__) + 1;
}

static inline int
f32 (int f1line)
{
  return f33 (f1line, __LINE__) + 2;
}

static inline int
f33 (int f1line, int f2line)
{
  uintptr_t addrs[20];
  struct sdata data;
  int f3line;
  int i;

  data.addrs = &addrs[0];
  data.index = 0;
  data.max = 20;
  data.failed = 0;

  f3line = __LINE__ + 1;
  i = backtrace_simple (state, 0, callback_two, error_callback_two, &data);

  if (i != 0)
    {
      fprintf (stderr, "test3: unexpected return value %d\n", i);
      data.failed = 1;
    }

  if (!data.failed)
    {
      struct info all[20];
      struct bdata bdata;

      bdata.all = &all[0];
      bdata.index = 0;
      bdata.max = 20;
      bdata.failed = 0;

      i = backtrace_pcinfo (state, addrs[0], callback_one, error_callback_one,
			    &bdata);
      if (i != 0)
	{
	  fprintf (stderr,
		   ("test4: unexpected return value "
		    "from backtrace_pcinfo %d\n"),
		   i);
	  bdata.failed = 1;
	}

      check ("test4", 0, all, f3line, "f33", "btest.c", &bdata.failed);
      check ("test4", 1, all, f2line, "f32", "btest.c", &bdata.failed);
      check ("test4", 2, all, f1line, "test4", "btest.c", &bdata.failed);

      if (bdata.failed)
	data.failed = 1;
    }

  printf ("%s: backtrace_simple inline\n", data.failed ? "FAIL" : "PASS");

  if (data.failed)
    ++failures;

  return failures;
}

static int test5 (void) __attribute__ ((unused));

int global = 1;

static int
test5 (void)
{
  struct symdata symdata;
  int i;
  uintptr_t addr = (uintptr_t) &global;

  if (sizeof (global) > 1)
    addr += 1;

  symdata.name = NULL;
  symdata.val = 0;
  symdata.size = 0;
  symdata.failed = 0;

  i = backtrace_syminfo (state, addr, callback_three,
			 error_callback_three, &symdata);
  if (i == 0)
    {
      fprintf (stderr,
	       "test5: unexpected return value from backtrace_syminfo %d\n",
	       i);
      symdata.failed = 1;
    }

  if (!symdata.failed)
    {
      if (symdata.name == NULL)
	{
	  fprintf (stderr, "test5: NULL syminfo name\n");
	  symdata.failed = 1;
	}
      else if (!(strncmp (symdata.name, "global", 6) == 0
		 && (symdata.name[6] == '\0'|| symdata.name[6] == '.')))
	{
	  fprintf (stderr,
		   "test5: unexpected syminfo name got %s expected %s\n",
		   symdata.name, "global");
	  symdata.failed = 1;
	}
      else if (symdata.val != (uintptr_t) &global)
	{
	  fprintf (stderr,
		   "test5: unexpected syminfo value got %lx expected %lx\n",
		   (unsigned long) symdata.val,
		   (unsigned long) (uintptr_t) &global);
	  symdata.failed = 1;
	}
      else if (symdata.size != sizeof (global) && symdata.size != 0)
	{
	  fprintf (stderr,
		   "test5: unexpected syminfo size got %lx expected %lx\n",
		   (unsigned long) symdata.size,
		   (unsigned long) sizeof (global));
	  symdata.failed = 1;
	}
    }

  printf ("%s: backtrace_syminfo variable\n",
	  symdata.failed ? "FAIL" : "PASS");

  if (symdata.failed)
    ++failures;

  return failures;
}

#define MIN_DESCRIPTOR 3
#define MAX_DESCRIPTOR 10

static int fstat_status[MAX_DESCRIPTOR];

/* Check files that are available.  */

static void
check_available_files (void)
{
  struct stat s;
  for (unsigned i = MIN_DESCRIPTOR; i < MAX_DESCRIPTOR; i++)
    fstat_status[i] = fstat (i, &s);
}

/* Check that are no files left open.  */

static void
check_open_files (void)
{
  for (unsigned i = MIN_DESCRIPTOR; i < MAX_DESCRIPTOR; i++)
    {
      if (fstat_status[i] != 0 && close (i) == 0)
	{
	  fprintf (stderr,
		   "ERROR: descriptor %d still open after tests complete\n",
		   i);
	  ++failures;
	}
    }
}

/* Run all the tests.  */

int
main (int argc ATTRIBUTE_UNUSED, char **argv)
{
  check_available_files ();

  state = backtrace_create_state (argv[0], BACKTRACE_SUPPORTS_THREADS,
				  error_callback_create, NULL);

#if BACKTRACE_SUPPORTED
  test1 ();
  test2 ();
  test3 ();
  test4 ();
#if BACKTRACE_SUPPORTS_DATA
  test5 ();
#endif
#endif

  check_open_files ();

  exit (failures ? EXIT_FAILURE : EXIT_SUCCESS);
}
