/* instrumented_alloc.c -- Memory allocation instrumented to fail when
   requested, for testing purposes.
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

/* Include all the header files of alloc here, to make sure they're not
   processed when including alloc.c below, such that the redefinitions of malloc
   and realloc are only effective in alloc.c itself.  This does not work for
   config.h, because it's not wrapped in "#ifndef CONFIG_H\n#define CONFIG_H"
   and "#endif" but that does not seem to be harmful.  */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <inttypes.h>

#include "backtrace.h"
#include "internal.h"

extern void *instrumented_malloc (size_t size);
extern void *instrumented_realloc (void *ptr, size_t size);

#define malloc instrumented_malloc
#define realloc instrumented_realloc
#include "alloc.c"
#undef malloc
#undef realloc

static uint64_t nr_allocs = 0;
static uint64_t fail_at_alloc = 0;

extern int at_fail_alloc_p (void);
extern uint64_t get_nr_allocs (void);
extern void set_fail_at_alloc (uint64_t);

void *
instrumented_malloc (size_t size)
{
  void *res;

  if (at_fail_alloc_p ())
    return NULL;

  res = malloc (size);
  if (res != NULL)
    nr_allocs++;

  return res;
}

void *
instrumented_realloc (void *ptr, size_t size)
{
  void *res;

  if (size != 0)
    {
      if (at_fail_alloc_p ())
	return NULL;
    }

  res = realloc (ptr, size);
  if (res != NULL)
    nr_allocs++;

  return res;
}

int
at_fail_alloc_p (void)
{
  return fail_at_alloc == nr_allocs + 1;
}

uint64_t
get_nr_allocs (void)
{
  return nr_allocs;
}

void
set_fail_at_alloc (uint64_t nr)
{
  fail_at_alloc = nr;
}
