/* xztest.c -- Test for libbacktrace LZMA decoder.
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

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_LIBLZMA
#include <lzma.h>
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
#define LIBLZMA_CLOCK_GETTIME_ARG CLOCK_PROCESS_CPUTIME_ID
#else
#define LIBLZMA_CLOCK_GETTIME_ARG CLOCK_REALTIME
#endif

/* Some tests for the local lzma inflation code.  */

struct lzma_test
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

static const struct lzma_test tests[] =
{
  {
    "empty",
    "",
    0,
    ("\xfd\x37\x7a\x58\x5a\x00\x00\x04\xe6\xd6\xb4\x46\x00\x00\x00\x00"
     "\x1c\xdf\x44\x21\x1f\xb6\xf3\x7d\x01\x00\x00\x00\x00\x04\x59\x5a"),
    32,
  },
  {
    "hello",
    "hello, world\n",
    0,
    ("\xfd\x37\x7a\x58\x5a\x00\x00\x04\xe6\xd6\xb4\x46\x02\x00\x21\x01"
     "\x16\x00\x00\x00\x74\x2f\xe5\xa3\x01\x00\x0c\x68\x65\x6c\x6c\x6f"
     "\x2c\x20\x77\x6f\x72\x6c\x64\x0a\x00\x00\x00\x00\x7b\x46\x5a\x81"
     "\xc9\x12\xb8\xea\x00\x01\x25\x0d\x71\x19\xc4\xb6\x1f\xb6\xf3\x7d"
     "\x01\x00\x00\x00\x00\x04\x59\x5a"),
    72,
  },
  {
    "goodbye",
    "goodbye, world",
    0,
    ("\xfd\x37\x7a\x58\x5a\x00\x00\x04\xe6\xd6\xb4\x46\x02\x00\x21\x01"
     "\x16\x00\x00\x00\x74\x2f\xe5\xa3\x01\x00\x0d\x67\x6f\x6f\x64\x62"
     "\x79\x65\x2c\x20\x77\x6f\x72\x6c\x64\x00\x00\x00\xf6\xf8\xa3\x33"
     "\x8c\x4e\xc9\x68\x00\x01\x26\x0e\x08\x1b\xe0\x04\x1f\xb6\xf3\x7d"
     "\x01\x00\x00\x00\x00\x04\x59\x5a"),
    72,
  },
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

      uncompressed = NULL;
      uncompressed_len = 0;
      if (!backtrace_uncompress_lzma (state,
				      ((const unsigned char *)
				       tests[i].compressed),
				      tests[i].compressed_len,
				      error_callback_compress, NULL,
				      &uncompressed, &uncompressed_len))
	{
	  fprintf (stderr, "test %s: uncompress failed\n", tests[i].name);
	  ++failures;
	}
      else
	{
	  size_t v;

	  v = tests[i].uncompressed_len;
	  if (v == 0)
	    v = strlen (tests[i].uncompressed);
	  if (uncompressed_len != v)
	    {
	      fprintf (stderr,
		       "test %s: got uncompressed length %zu, want %zu\n",
		       tests[i].name, uncompressed_len, v);
	      ++failures;
	    }
	  else if (v > 0 && memcmp (tests[i].uncompressed, uncompressed, v) != 0)
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
	    printf ("PASS: lzma %s\n", tests[i].name);

	  backtrace_free (state, uncompressed, uncompressed_len,
			  error_callback_compress, NULL);
	}
    }
}

#if HAVE_LIBLZMA

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
#if HAVE_LIBLZMA
  unsigned char *orig_buf;
  size_t orig_bufsize;
  size_t i;
  lzma_stream initial_stream = LZMA_STREAM_INIT;
  lzma_stream stream;
  unsigned char *compressed_buf;
  size_t compressed_bufsize;
  unsigned char *uncompressed_buf;
  size_t uncompressed_bufsize;
  unsigned char *spare_buf;
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
      printf ("UNSUPPORTED: lzma large\n");
      return;
    }

  stream = initial_stream;
  r =  lzma_easy_encoder (&stream, 6, LZMA_CHECK_CRC32);
  if (r != LZMA_OK)
    {
      fprintf (stderr, "lzma_easy_encoder failed: %d\n", r);
      goto fail;
    }

  compressed_bufsize = orig_bufsize + 100;
  compressed_buf = malloc (compressed_bufsize);
  if (compressed_buf == NULL)
    {
      perror ("malloc");
      goto fail;
    }

  stream.next_in = orig_buf;
  stream.avail_in = orig_bufsize;
  stream.next_out = compressed_buf;
  stream.avail_out = compressed_bufsize;

  do
    {
      r = lzma_code (&stream, LZMA_FINISH);
      if (r != LZMA_OK && r != LZMA_STREAM_END)
	{
	  fprintf (stderr, "lzma_code failed: %d\n", r);
	  goto fail;
	}
    }
  while (r != LZMA_STREAM_END);

  compressed_bufsize = stream.total_out;

  if (!backtrace_uncompress_lzma (state, (unsigned char *) compressed_buf,
				  compressed_bufsize,
				  error_callback_compress, NULL,
				  &uncompressed_buf, &uncompressed_bufsize))
    {
      fprintf (stderr, "lzma large: backtrace_uncompress_lzma failed\n");
      goto fail;
    }

  if (uncompressed_bufsize != orig_bufsize)
    {
      fprintf (stderr,
	       "lzma large: got uncompressed length %zu, want %zu\n",
	       uncompressed_bufsize, orig_bufsize);
      goto fail;
    }

  if (memcmp (uncompressed_buf, orig_buf, uncompressed_bufsize) != 0)
    {
      fprintf (stderr, "lzma large: uncompressed data mismatch\n");
      goto fail;
    }

  printf ("PASS: lzma large\n");

  spare_buf = malloc (orig_bufsize);
  if (spare_buf == NULL)
    {
      perror ("malloc");
      goto fail;
    }

  for (i = 0; i < trials; ++i)
    {
      cid = LIBLZMA_CLOCK_GETTIME_ARG;
      if (clock_gettime (cid, &ts1) < 0)
	{
	  if (errno == EINVAL)
	    return;
	  perror ("clock_gettime");
	  return;
	}

      if (!backtrace_uncompress_lzma (state,
				      (unsigned char *) compressed_buf,
				      compressed_bufsize,
				      error_callback_compress, NULL,
				      &uncompressed_buf,
				      &uncompressed_bufsize))
	{
	  fprintf (stderr,
		   ("lzma large: "
		    "benchmark backtrace_uncompress_lzma failed\n"));
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

      stream = initial_stream;

      r = lzma_auto_decoder (&stream, UINT64_MAX, 0);
      if (r != LZMA_OK)
	{
	  fprintf (stderr, "lzma_stream_decoder failed: %d\n", r);
	  goto fail;
	}

      stream.next_in = compressed_buf;
      stream.avail_in = compressed_bufsize;
      stream.next_out = spare_buf;
      stream.avail_out = orig_bufsize;

      if (clock_gettime (cid, &ts1) < 0)
	{
	  perror("clock_gettime");
	  return;
	}

      do
	{
	  r = lzma_code (&stream, LZMA_FINISH);
	  if (r != LZMA_OK && r != LZMA_STREAM_END)
	    {
	      fprintf (stderr, "lzma_code failed: %d\n", r);
	      goto fail;
	    }
	}
      while (r != LZMA_STREAM_END);

      if (clock_gettime (cid, &ts2) < 0)
	{
	  perror ("clock_gettime");
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
  printf ("liblzma  : %zu ns\n", ztime);
  printf ("ratio    : %g\n", (double) ztime / (double) ctime);

  return;

 fail:
  printf ("FAIL: lzma large\n");
  ++failures;

  if (orig_buf != NULL)
    free (orig_buf);
  if (compressed_buf != NULL)
    free (compressed_buf);
  if (uncompressed_buf != NULL)
    free (uncompressed_buf);

#else /* !HAVE_LIBLZMA */

 printf ("UNSUPPORTED: lzma large\n");

#endif /* !HAVE_LIBLZMA */
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
