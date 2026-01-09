/* ztest.c -- Test for libbacktrace zstd code.
   Copyright (C) 2022-2024 Free Software Foundation, Inc.
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

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

#include "backtrace.h"
#include "backtrace-supported.h"

#include "internal.h"
#include "testlib.h"

#ifndef HAVE_CLOCK_GETTIME

typedef int xclockid_t;

static int
xclock_gettime (xclockid_t id ATTRIBUTE_UNUSED,
		struct timespec *ts ATTRIBUTE_UNUSED)
{
  errno = EINVAL;
  return -1;
}

#define clockid_t xclockid_t
#define clock_gettime xclock_gettime
#undef CLOCK_REALTIME
#define CLOCK_REALTIME 0

#endif /* !defined(HAVE_CLOCK_GETTIME) */

#ifdef CLOCK_PROCESS_CPUTIME_ID
#define ZSTD_CLOCK_GETTIME_ARG CLOCK_PROCESS_CPUTIME_ID
#else
#define ZSTD_CLOCK_GETTIME_ARG CLOCK_REALTIME
#endif

/* Some tests for the local zstd inflation code.  */

struct zstd_test
{
  const char *name;
  const char *uncompressed;
  size_t uncompressed_len;
  const char *compressed;
  size_t compressed_len;
};

/* Error callback.  */

static void
error_callback_compress (void *vdata ATTRIBUTE_UNUSED, const char *msg,
			 int errnum)
{
  fprintf (stderr, "%s", msg);
  if (errnum > 0)
    fprintf (stderr, ": %s", strerror (errnum));
  fprintf (stderr, "\n");
  exit (EXIT_FAILURE);
}

static const struct zstd_test tests[] =
{
  {
    "empty",
    "",
    0,
    "\x28\xb5\x2f\xfd\x24\x00\x01\x00\x00\x99\xe9\xd8\x51",
    13,
  },
  {
    "hello",
    "hello, world\n",
    0,
    ("\x28\xb5\x2f\xfd\x24\x0d\x69\x00\x00\x68\x65\x6c\x6c\x6f\x2c\x20"
     "\x77\x6f\x72\x6c\x64\x0a\x4c\x1f\xf9\xf1"),
    26,
  },
  {
    "goodbye",
    "goodbye, world",
    0,
    ("\x28\xb5\x2f\xfd\x24\x0e\x71\x00\x00\x67\x6f\x6f\x64\x62\x79\x65"
     "\x2c\x20\x77\x6f\x72\x6c\x64\x61\x7b\x4b\x83"),
    27,
  },
  {
    "ranges",
    ("\xcc\x11\x00\x00\x00\x00\x00\x00\xd5\x13\x00\x00\x00\x00\x00\x00"
     "\x1c\x14\x00\x00\x00\x00\x00\x00\x72\x14\x00\x00\x00\x00\x00\x00"
     "\x9d\x14\x00\x00\x00\x00\x00\x00\xd5\x14\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\xfb\x12\x00\x00\x00\x00\x00\x00\x09\x13\x00\x00\x00\x00\x00\x00"
     "\x0c\x13\x00\x00\x00\x00\x00\x00\xcb\x13\x00\x00\x00\x00\x00\x00"
     "\x29\x14\x00\x00\x00\x00\x00\x00\x4e\x14\x00\x00\x00\x00\x00\x00"
     "\x9d\x14\x00\x00\x00\x00\x00\x00\xd5\x14\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\xfb\x12\x00\x00\x00\x00\x00\x00\x09\x13\x00\x00\x00\x00\x00\x00"
     "\x67\x13\x00\x00\x00\x00\x00\x00\xcb\x13\x00\x00\x00\x00\x00\x00"
     "\x9d\x14\x00\x00\x00\x00\x00\x00\xd5\x14\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x5f\x0b\x00\x00\x00\x00\x00\x00\x6c\x0b\x00\x00\x00\x00\x00\x00"
     "\x7d\x0b\x00\x00\x00\x00\x00\x00\x7e\x0c\x00\x00\x00\x00\x00\x00"
     "\x38\x0f\x00\x00\x00\x00\x00\x00\x5c\x0f\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x83\x0c\x00\x00\x00\x00\x00\x00\xfa\x0c\x00\x00\x00\x00\x00\x00"
     "\xfd\x0d\x00\x00\x00\x00\x00\x00\xef\x0e\x00\x00\x00\x00\x00\x00"
     "\x14\x0f\x00\x00\x00\x00\x00\x00\x38\x0f\x00\x00\x00\x00\x00\x00"
     "\x9f\x0f\x00\x00\x00\x00\x00\x00\xac\x0f\x00\x00\x00\x00\x00\x00"
     "\xdb\x0f\x00\x00\x00\x00\x00\x00\xff\x0f\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\xfd\x0d\x00\x00\x00\x00\x00\x00\xd8\x0e\x00\x00\x00\x00\x00\x00"
     "\x9f\x0f\x00\x00\x00\x00\x00\x00\xac\x0f\x00\x00\x00\x00\x00\x00"
     "\xdb\x0f\x00\x00\x00\x00\x00\x00\xff\x0f\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\xfa\x0c\x00\x00\x00\x00\x00\x00\xea\x0d\x00\x00\x00\x00\x00\x00"
     "\xef\x0e\x00\x00\x00\x00\x00\x00\x14\x0f\x00\x00\x00\x00\x00\x00"
     "\x5c\x0f\x00\x00\x00\x00\x00\x00\x9f\x0f\x00\x00\x00\x00\x00\x00"
     "\xac\x0f\x00\x00\x00\x00\x00\x00\xdb\x0f\x00\x00\x00\x00\x00\x00"
     "\xff\x0f\x00\x00\x00\x00\x00\x00\x2c\x10\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x60\x11\x00\x00\x00\x00\x00\x00\xd1\x16\x00\x00\x00\x00\x00\x00"
     "\x40\x0b\x00\x00\x00\x00\x00\x00\x2c\x10\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x7a\x00\x00\x00\x00\x00\x00\x00\xb6\x00\x00\x00\x00\x00\x00\x00"
     "\x9f\x01\x00\x00\x00\x00\x00\x00\xa7\x01\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
     "\x7a\x00\x00\x00\x00\x00\x00\x00\xa9\x00\x00\x00\x00\x00\x00\x00"
     "\x9f\x01\x00\x00\x00\x00\x00\x00\xa7\x01\x00\x00\x00\x00\x00\x00"
     "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"),
    672,
    ("\x28\xb5\x2f\xfd\x64\xa0\x01\x2d\x05\x00\xc4\x04\xcc\x11\x00\xd5"
     "\x13\x00\x1c\x14\x00\x72\x9d\xd5\xfb\x12\x00\x09\x0c\x13\xcb\x13"
     "\x29\x4e\x67\x5f\x0b\x6c\x0b\x7d\x0b\x7e\x0c\x38\x0f\x5c\x0f\x83"
     "\x0c\xfa\x0c\xfd\x0d\xef\x0e\x14\x38\x9f\x0f\xac\x0f\xdb\x0f\xff"
     "\x0f\xd8\x9f\xac\xdb\xff\xea\x5c\x2c\x10\x60\xd1\x16\x40\x0b\x7a"
     "\x00\xb6\x00\x9f\x01\xa7\x01\xa9\x36\x20\xa0\x83\x14\x34\x63\x4a"
     "\x21\x70\x8c\x07\x46\x03\x4e\x10\x62\x3c\x06\x4e\xc8\x8c\xb0\x32"
     "\x2a\x59\xad\xb2\xf1\x02\x82\x7c\x33\xcb\x92\x6f\x32\x4f\x9b\xb0"
     "\xa2\x30\xf0\xc0\x06\x1e\x98\x99\x2c\x06\x1e\xd8\xc0\x03\x56\xd8"
     "\xc0\x03\x0f\x6c\xe0\x01\xf1\xf0\xee\x9a\xc6\xc8\x97\x99\xd1\x6c"
     "\xb4\x21\x45\x3b\x10\xe4\x7b\x99\x4d\x8a\x36\x64\x5c\x77\x08\x02"
     "\xcb\xe0\xce"),
    179,
  }
};

/* Test the hand coded samples.  */

static void
test_samples (struct backtrace_state *state)
{
  size_t i;

  for (i = 0; i < sizeof tests / sizeof tests[0]; ++i)
    {
      unsigned char *uncompressed;
      size_t uncompressed_len;

      uncompressed_len = tests[i].uncompressed_len;
      if (uncompressed_len == 0)
	uncompressed_len = strlen (tests[i].uncompressed);

      uncompressed = (unsigned char *) malloc (uncompressed_len);
      if (uncompressed == NULL)
	{
	  perror ("malloc");
	  fprintf (stderr, "test %s: uncompress failed\n", tests[i].name);
	  ++failures;
	  continue;
	}

      if (!backtrace_uncompress_zstd (state,
				      ((const unsigned char *)
				       tests[i].compressed),
				      tests[i].compressed_len,
				      error_callback_compress, NULL,
				      uncompressed, uncompressed_len))
	{
	  fprintf (stderr, "test %s: uncompress failed\n", tests[i].name);
	  ++failures;
	}
      else
	{
	  if (memcmp (tests[i].uncompressed, uncompressed, uncompressed_len)
	      != 0)
	    {
	      size_t j;

	      fprintf (stderr, "test %s: uncompressed data mismatch\n",
		       tests[i].name);
	      for (j = 0; j < uncompressed_len; ++j)
		if (tests[i].uncompressed[j] != uncompressed[j])
		  fprintf (stderr, "  %zu: got %#x want %#x\n", j,
			   uncompressed[j], tests[i].uncompressed[j]);
	      ++failures;
	    }
	  else
	    printf ("PASS: uncompress %s\n", tests[i].name);
	}

      free (uncompressed);
    }
}

#ifdef HAVE_ZSTD

/* Given a set of TRIALS timings, discard the lowest and highest
   values and return the mean average of the rest.  */

static size_t
average_time (const size_t *times, size_t trials)
{
  size_t imax;
  size_t max;
  size_t imin;
  size_t min;
  size_t i;
  size_t sum;

  imin = 0;
  imax = 0;
  min = times[0];
  max = times[0];
  for (i = 1; i < trials; ++i)
    {
      if (times[i] < min)
	{
	  imin = i;
	  min = times[i];
	}
      if (times[i] > max)
	{
	  imax = i;
	  max = times[i];
	}
    }

  sum = 0;
  for (i = 0; i < trials; ++i)
    {
      if (i != imax && i != imin)
	sum += times[i];
    }
  return sum / (trials - 2);
}

#endif

/* Test a larger text, if available.  */

static void
test_large (struct backtrace_state *state ATTRIBUTE_UNUSED)
{
#ifdef HAVE_ZSTD
  unsigned char *orig_buf;
  size_t orig_bufsize;
  size_t i;
  char *compressed_buf;
  size_t compressed_bufsize;
  size_t compressed_size;
  unsigned char *uncompressed_buf;
  size_t r;
  clockid_t cid;
  struct timespec ts1;
  struct timespec ts2;
  size_t ctime;
  size_t ztime;
  const size_t trials = 16;
  size_t ctimes[16];
  size_t ztimes[16];
  static const char * const names[] = {
    "Isaac.Newton-Opticks.txt",
    "../libgo/go/testdata/Isaac.Newton-Opticks.txt",
  };

  orig_buf = NULL;
  orig_bufsize = 0;
  uncompressed_buf = NULL;
  compressed_buf = NULL;

  for (i = 0; i < sizeof names / sizeof names[0]; ++i)
    {
      size_t len;
      char *namebuf;
      FILE *e;
      struct stat st;
      char *rbuf;
      size_t got;

      len = strlen (SRCDIR) + strlen (names[i]) + 2;
      namebuf = malloc (len);
      if (namebuf == NULL)
	{
	  perror ("malloc");
	  goto fail;
	}
      snprintf (namebuf, len, "%s/%s", SRCDIR, names[i]);
      e = fopen (namebuf, "r");
      free (namebuf);
      if (e == NULL)
	continue;
      if (fstat (fileno (e), &st) < 0)
	{
	  perror ("fstat");
	  fclose (e);
	  continue;
	}
      rbuf = malloc (st.st_size);
      if (rbuf == NULL)
	{
	  perror ("malloc");
	  goto fail;
	}
      got = fread (rbuf, 1, st.st_size, e);
      fclose (e);
      if (got > 0)
	{
	  orig_buf = (unsigned char *) rbuf;
	  orig_bufsize = got;
	  break;
	}
      free (rbuf);
    }

  if (orig_buf == NULL)
    {
      /* We couldn't find an input file.  */
      printf ("UNSUPPORTED: zstd large\n");
      return;
    }

  compressed_bufsize = ZSTD_compressBound (orig_bufsize);
  compressed_buf = malloc (compressed_bufsize);
  if (compressed_buf == NULL)
    {
      perror ("malloc");
      goto fail;
    }

  r = ZSTD_compress (compressed_buf, compressed_bufsize,
		     orig_buf, orig_bufsize,
		     ZSTD_CLEVEL_DEFAULT);
  if (ZSTD_isError (r))
    {
      fprintf (stderr, "zstd compress failed: %s\n", ZSTD_getErrorName (r));
      goto fail;
    }
  compressed_size = r;

  uncompressed_buf = malloc (orig_bufsize);
  if (uncompressed_buf == NULL)
    {
      perror ("malloc");
      goto fail;
    }

  if (!backtrace_uncompress_zstd (state, (unsigned char *) compressed_buf,
				  compressed_size,
				  error_callback_compress, NULL,
				  uncompressed_buf, orig_bufsize))
    {
      fprintf (stderr, "zstd large: backtrace_uncompress_zstd failed\n");
      goto fail;
    }

  if (memcmp (uncompressed_buf, orig_buf, orig_bufsize) != 0)
    {
      size_t j;

      fprintf (stderr, "zstd large: uncompressed data mismatch\n");
      for (j = 0; j < orig_bufsize; ++j)
	if (orig_buf[j] != uncompressed_buf[j])
	  fprintf (stderr, "  %zu: got %#x want %#x\n", j,
		   uncompressed_buf[j], orig_buf[j]);
      goto fail;
    }

  printf ("PASS: zstd large\n");

  for (i = 0; i < trials; ++i)
    {
      cid = ZSTD_CLOCK_GETTIME_ARG;
      if (clock_gettime (cid, &ts1) < 0)
	{
	  if (errno == EINVAL)
	    return;
	  perror ("clock_gettime");
	  return;
	}

      if (!backtrace_uncompress_zstd (state,
				      (unsigned char *) compressed_buf,
				      compressed_size,
				      error_callback_compress, NULL,
				      uncompressed_buf,
				      orig_bufsize))
	{
	  fprintf (stderr,
		   ("zstd large: "
		    "benchmark backtrace_uncompress_zstd failed\n"));
	  return;
	}

      if (clock_gettime (cid, &ts2) < 0)
	{
	  perror ("clock_gettime");
	  return;
	}

      ctime = (ts2.tv_sec - ts1.tv_sec) * 1000000000;
      ctime += ts2.tv_nsec - ts1.tv_nsec;
      ctimes[i] = ctime;

      if (clock_gettime (cid, &ts1) < 0)
	{
	  perror("clock_gettime");
	  return;
	}

      r = ZSTD_decompress (uncompressed_buf, orig_bufsize,
			   compressed_buf, compressed_size);

      if (clock_gettime (cid, &ts2) < 0)
	{
	  perror ("clock_gettime");
	  return;
	}

      if (ZSTD_isError (r))
	{
	  fprintf (stderr,
		   "zstd large: benchmark zlib uncompress failed: %s\n",
		   ZSTD_getErrorName (r));
	  return;
	}

      ztime = (ts2.tv_sec - ts1.tv_sec) * 1000000000;
      ztime += ts2.tv_nsec - ts1.tv_nsec;
      ztimes[i] = ztime;
    }

  /* Toss the highest and lowest times and average the rest.  */
  ctime = average_time (ctimes, trials);
  ztime = average_time (ztimes, trials);

  printf ("backtrace: %zu ns\n", ctime);
  printf ("zstd     : %zu ns\n", ztime);
  printf ("ratio    : %g\n", (double) ztime / (double) ctime);

  return;

 fail:
  printf ("FAIL: zstd large\n");
  ++failures;

  if (orig_buf != NULL)
    free (orig_buf);
  if (compressed_buf != NULL)
    free (compressed_buf);
  if (uncompressed_buf != NULL)
    free (uncompressed_buf);

#else /* !HAVE_ZSTD */

 printf ("UNSUPPORTED: zstd large\n");

#endif /* !HAVE_ZSTD */
}

int
main (int argc ATTRIBUTE_UNUSED, char **argv)
{
  struct backtrace_state *state;

  state = backtrace_create_state (argv[0], BACKTRACE_SUPPORTS_THREADS,
				  error_callback_create, NULL);

  test_samples (state);
  test_large (state);

  exit (failures != 0 ? EXIT_FAILURE : EXIT_SUCCESS);
}
