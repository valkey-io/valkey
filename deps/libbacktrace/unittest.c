/* unittest.c -- Test for libbacktrace library
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

#include "internal.h"

static unsigned count;

static void
error_callback (void *vdata ATTRIBUTE_UNUSED, const char *msg ATTRIBUTE_UNUSED,
		int errnum ATTRIBUTE_UNUSED)
{
  ++count;
}

static int
test1 (void)
{
  int res;
  int failed;

  struct backtrace_vector vec;

  memset (&vec, 0, sizeof vec);

  backtrace_vector_grow (state, 100, error_callback, NULL, &vec);
  vec.alc += vec.size;
  vec.size = 0;

  count = 0;
  res = backtrace_vector_release (state, &vec, error_callback, NULL);
  failed = res != 1 || count != 0 || vec.base != NULL;

  printf ("%s: unittest backtrace_vector_release size == 0\n",
	  failed ? "FAIL": "PASS");

  if (failed)
    ++failures;

  return failures;
}

int
main (int argc ATTRIBUTE_UNUSED, char **argv)
{
  state = backtrace_create_state (argv[0], BACKTRACE_SUPPORTS_THREADS,
				  error_callback_create, NULL);

  test1 ();

  exit (failures ? EXIT_FAILURE : EXIT_SUCCESS);
}
