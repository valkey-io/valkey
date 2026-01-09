/* ztest.c -- Test for libbacktrace inflate code.
   Copyright (C) 2017-2024 Free Software Foundation, Inc.
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

#ifdef HAVE_ZLIB
#include <zlib.h>
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
#define ZLIB_CLOCK_GETTIME_ARG CLOCK_PROCESS_CPUTIME_ID
#else
#define ZLIB_CLOCK_GETTIME_ARG CLOCK_REALTIME
#endif

/* Some tests for the local zlib inflation code.  */

struct zlib_test
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

static const struct zlib_test tests[] =
{
  {
    "empty",
    "",
    0,
    "\x78\x9c\x03\x00\x00\x00\x00\x01",
    8,
  },
  {
    "hello",
    "hello, world\n",
    0,
    ("\x78\x9c\xca\x48\xcd\xc9\xc9\xd7\x51\x28\xcf"
     "\x2f\xca\x49\xe1\x02\x04\x00\x00\xff\xff\x21\xe7\x04\x93"),
    25,
  },
  {
    "goodbye",
    "goodbye, world",
    0,
    ("\x78\x9c\x4b\xcf\xcf\x4f\x49\xaa"
     "\x4c\xd5\x51\x28\xcf\x2f\xca\x49"
     "\x01\x00\x28\xa5\x05\x5e"),
    22,
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
    ("\x78\x9c\x3b\x23\xc8\x00\x06\x57\x85\x21\xb4\x8c\x08\x84\x2e\x82"
     "\xd2\x73\xa1\xf4\x55\x28\x8d\x0e\x7e\x0b\x41\x68\x4e\xa8\x7e\x1e"
     "\x28\x7d\x1a\x4a\x6b\x42\xf5\xf9\x91\x69\x5e\x3a\x9a\x79\x84\xf4"
     "\xc7\x73\x43\xe8\x1c\x28\x5d\x0b\xa5\xeb\x78\x20\xb4\x05\x3f\x84"
     "\x8e\xe1\xc7\xae\xbf\x19\xaa\xee\x17\x94\xfe\xcb\x0b\xa1\xdf\xf3"
     "\x41\x68\x11\x7e\x54\x73\xe6\x43\xe9\x35\x50\xfa\x36\x94\xfe\x8f"
     "\xc3\x7c\x98\x79\x37\xf8\xc8\xd3\x0f\x73\xd7\x2b\x1c\xee\x8a\x21"
     "\xd2\x5d\x3a\x02\xd8\xcd\x4f\x80\xa6\x87\x8b\x62\x10\xda\x81\x1b"
     "\xbf\xfa\x2a\x28\xbd\x0d\x4a\xcf\x67\x84\xd0\xcb\x19\xf1\xab\x5f"
     "\x49\xa4\x7a\x00\x48\x97\x29\xd4"),
    152,
  }
};

/* Test the hand coded samples.  */

static void
test_samples (struct backtrace_state *state)
{
  size_t i;

  for (i = 0; i < sizeof tests / sizeof tests[0]; ++i)
    {
      char *p;
      size_t v;
      size_t j;
      unsigned char *uncompressed;
      size_t uncompressed_len;

      p = malloc (12 + tests[i].compressed_len);
      memcpy (p, "ZLIB", 4);
      v = tests[i].uncompressed_len;
      if (v == 0)
	v = strlen (tests[i].uncompressed);
      for (j = 0; j < 8; ++j)
	p[j + 4] = (v >> ((7 - j) * 8)) & 0xff;
      memcpy (p + 12, tests[i].compressed, tests[i].compressed_len);
      uncompressed = NULL;
      uncompressed_len = 0;
      if (!backtrace_uncompress_zdebug (state, (unsigned char *) p,
					tests[i].compressed_len + 12,
					error_callback_compress, NULL,
					&uncompressed, &uncompressed_len))
	{
	  fprintf (stderr, "test %s: uncompress failed\n", tests[i].name);
	  ++failures;
	}
      else
	{
	  if (uncompressed_len != v)
	    {
	      fprintf (stderr,
		       "test %s: got uncompressed length %zu, want %zu\n",
		       tests[i].name, uncompressed_len, v);
	      ++failures;
	    }
	  else if (memcmp (tests[i].uncompressed, uncompressed, v) != 0)
	    {
	      size_t j;

	      fprintf (stderr, "test %s: uncompressed data mismatch\n",
		       tests[i].name);
	      for (j = 0; j < v; ++j)
		if (tests[i].uncompressed[j] != uncompressed[j])
		  fprintf (stderr, "  %zu: got %#x want %#x\n", j,
			   uncompressed[j], tests[i].uncompressed[j]);
	      ++failures;
	    }
	  else
	    printf ("PASS: inflate %s\n", tests[i].name);

	  backtrace_free (state, uncompressed, uncompressed_len,
			  error_callback_compress, NULL);
	}
    }
}

#ifdef HAVE_ZLIB

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
#ifdef HAVE_ZLIB
  unsigned char *orig_buf;
  size_t orig_bufsize;
  size_t i;
  char *compressed_buf;
  size_t compressed_bufsize;
  unsigned long compress_sizearg;
  unsigned char *uncompressed_buf;
  size_t uncompressed_bufsize;
  int r;
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
      printf ("UNSUPPORTED: inflate large\n");
      return;
    }

  compressed_bufsize = compressBound (orig_bufsize) + 12;
  compressed_buf = malloc (compressed_bufsize);
  if (compressed_buf == NULL)
    {
      perror ("malloc");
      goto fail;
    }

  compress_sizearg = compressed_bufsize - 12;
  r = compress ((unsigned char *) compressed_buf + 12, &compress_sizearg,
		orig_buf, orig_bufsize);
  if (r != Z_OK)
    {
      fprintf (stderr, "zlib compress failed: %d\n", r);
      goto fail;
    }

  compressed_bufsize = compress_sizearg + 12;

  /* Prepare the header that our library expects.  */
  memcpy (compressed_buf, "ZLIB", 4);
  for (i = 0; i < 8; ++i)
    compressed_buf[i + 4] = (orig_bufsize >> ((7 - i) * 8)) & 0xff;

  uncompressed_buf = malloc (orig_bufsize);
  if (uncompressed_buf == NULL)
    {
      perror ("malloc");
      goto fail;
    }
  uncompressed_bufsize = orig_bufsize;

  if (!backtrace_uncompress_zdebug (state, (unsigned char *) compressed_buf,
				    compressed_bufsize,
				    error_callback_compress, NULL,
				    &uncompressed_buf, &uncompressed_bufsize))
    {
      fprintf (stderr, "inflate large: backtrace_uncompress_zdebug failed\n");
      goto fail;
    }

  if (uncompressed_bufsize != orig_bufsize)
    {
      fprintf (stderr,
	       "inflate large: got uncompressed length %zu, want %zu\n",
	       uncompressed_bufsize, orig_bufsize);
      goto fail;
    }

  if (memcmp (uncompressed_buf, orig_buf, uncompressed_bufsize) != 0)
    {
      fprintf (stderr, "inflate large: uncompressed data mismatch\n");
      goto fail;
    }

  printf ("PASS: inflate large\n");

  for (i = 0; i < trials; ++i)
    {
      unsigned long uncompress_sizearg;

      cid = ZLIB_CLOCK_GETTIME_ARG;
      if (clock_gettime (cid, &ts1) < 0)
	{
	  if (errno == EINVAL)
	    return;
	  perror ("clock_gettime");
	  return;
	}

      if (!backtrace_uncompress_zdebug (state,
					(unsigned char *) compressed_buf,
					compressed_bufsize,
					error_callback_compress, NULL,
					&uncompressed_buf,
					&uncompressed_bufsize))
	{
	  fprintf (stderr,
		   ("inflate large: "
		    "benchmark backtrace_uncompress_zdebug failed\n"));
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

      uncompress_sizearg = uncompressed_bufsize;
      r = uncompress ((unsigned char *) uncompressed_buf, &uncompress_sizearg,
		      (unsigned char *) compressed_buf + 12,
		      compressed_bufsize - 12);

      if (clock_gettime (cid, &ts2) < 0)
	{
	  perror ("clock_gettime");
	  return;
	}

      if (r != Z_OK)
	{
	  fprintf (stderr,
		   "inflate large: benchmark zlib uncompress failed: %d\n",
		   r);
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
  printf ("zlib     : %zu ns\n", ztime);
  printf ("ratio    : %g\n", (double) ztime / (double) ctime);

  return;

 fail:
  printf ("FAIL: inflate large\n");
  ++failures;

  if (orig_buf != NULL)
    free (orig_buf);
  if (compressed_buf != NULL)
    free (compressed_buf);
  if (uncompressed_buf != NULL)
    free (uncompressed_buf);

#else /* !HAVE_ZLIB */

 printf ("UNSUPPORTED: inflate large\n");

#endif /* !HAVE_ZLIB */
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
