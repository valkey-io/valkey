/* mtest.c -- Minidebug test for libbacktrace library
   Copyright (C) 2020-2024 Free Software Foundation, Inc.
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

/* This program tests using libbacktrace with a program that uses the
   minidebuginfo format in a .gnu_debugdata section.  See
   https://sourceware.org/gdb/current/onlinedocs/gdb/MiniDebugInfo.html
   for a bit more information about minidebuginfo.  What is relevant
   for libbacktrace is that we have just a symbol table, with no debug
   info, so we should be able to do a function backtrace, but we can't
   do a file/line backtrace.  */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "backtrace.h"
#include "backtrace-supported.h"

#include "testlib.h"

static int test1 (void) __attribute__ ((noinline, noclone, optnone, unused));
static int f2 (int) __attribute__ ((noinline, noclone));
static int f3 (int, int) __attribute__ ((noinline, noclone));

/* Collected PC values.  */

static uintptr_t addrs[20];

/* The backtrace callback function.  This is like callback_one in
   testlib.c, but it saves the PC also.  */

static int
callback_mtest (void *vdata, uintptr_t pc, const char *filename, int lineno,
		const char *function)
{
  struct bdata *data = (struct bdata *) vdata;

  if (data->index >= sizeof addrs / sizeof addrs[0])
    {
      fprintf (stderr, "callback_mtest: callback called too many times\n");
      data->failed = 1;
      return 1;
    }

  addrs[data->index] = pc;

  return callback_one (vdata, pc, filename, lineno, function);
}

/* Test the backtrace function with non-inlined functions.  (We don't
   test with inlined functions because they won't work with minidebug
   anyhow.)  */

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
f3 (int f1line __attribute__ ((unused)), int f2line __attribute__ ((unused)))
{
  struct info all[20];
  struct bdata data;
  int i;
  size_t j;

  data.all = &all[0];
  data.index = 0;
  data.max = 20;
  data.failed = 0;

  i = backtrace_full (state, 0, callback_mtest, error_callback_one, &data);

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

  /* When using minidebug we don't expect the function name here.  */

  for (j = 0; j < 3 && j < data.index; j++)
    {
      if (all[j].function == NULL)
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
		       ("test1: [%zu], unexpected return value from "
			"backtrace_syminfo %d\n"),
		       j, i);
	      data.failed = 1;
	    }
	  else if (symdata.name == NULL)
	    {
	      fprintf (stderr, "test1: [%zu]: syminfo did not find name\n", j);
	      data.failed = 1;
	    }
	  else
	    all[j].function = strdup (symdata.name);
	}
    }

  if (data.index > 0)
    {
      if (all[0].function == NULL)
	{
	  fprintf (stderr, "test1: [0]: missing function name\n");
	  data.failed = 1;
	}
      else if (strcmp (all[0].function, "f3") != 0)
	{
	  fprintf (stderr, "test1: [0]: got %s expected %s\n",
		   all[0].function, "f3");
	  data.failed = 1;
	}
    }

  if (data.index > 1)
    {
      if (all[1].function == NULL)
	{
	  fprintf (stderr, "test1: [1]: missing function name\n");
	  data.failed = 1;
	}
      else if (strcmp (all[1].function, "f2") != 0)
	{
	  fprintf (stderr, "test1: [1]: got %s expected %s\n",
		   all[0].function, "f2");
	  data.failed = 1;
	}
    }

  if (data.index > 2)
    {
      if (all[2].function == NULL)
	{
	  fprintf (stderr, "test1: [2]: missing function name\n");
	  data.failed = 1;
	}
      else if (strcmp (all[2].function, "test1") != 0)
	{
	  fprintf (stderr, "test1: [2]: got %s expected %s\n",
		   all[0].function, "test1");
	  data.failed = 1;
	}
    }

  printf ("%s: backtrace_full noinline\n", data.failed ? "FAIL" : "PASS");

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
f23 (int f1line __attribute__ ((unused)), int f2line __attribute__ ((unused)))
{
  uintptr_t addrs[20];
  struct sdata data;
  int i;

  data.addrs = &addrs[0];
  data.index = 0;
  data.max = 20;
  data.failed = 0;

  i = backtrace_simple (state, 0, callback_two, error_callback_two, &data);

  if (i != 0)
    {
      fprintf (stderr, "test3: unexpected return value %d\n", i);
      data.failed = 1;
    }

  if (!data.failed)
    {
      int j;

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

int test5 (void) __attribute__ ((unused));

int global = 1;

int
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

int
main (int argc ATTRIBUTE_UNUSED, char **argv)
{
  state = backtrace_create_state (argv[0], BACKTRACE_SUPPORTS_THREADS,
				  error_callback_create, NULL);

#if BACKTRACE_SUPPORTED
  test1 ();
  test3 ();
#if BACKTRACE_SUPPORTS_DATA
  test5 ();
#endif
#endif

  exit (failures ? EXIT_FAILURE : EXIT_SUCCESS);
}
