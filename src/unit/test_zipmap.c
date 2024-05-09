#include "../zipmap.c"
#include "test_help.h"

int test_zipmapLookupLargeKey(int argc, char **argv, int flags) {
  UNUSED(argc);
  UNUSED(argv);
  UNUSED(flags);
  unsigned char buf[512];
  unsigned char *value;
  unsigned int vlen, i;
  unsigned char *zm;
  for (i = 0; i < 512; i++)
    buf[i] = 'a';

  zm = zipmapNew();
  zm = zipmapSet(zm, buf, 512, (unsigned char *)"long", 4, NULL);
  TEST_ASSERT(zipmapGet(zm, buf, 512, &value, &vlen) == 1);
  zfree(zm);
  return 0;
}

int test_zipmapPerformDirectLookup(int argc, char **argv, int flags) {
  UNUSED(argc);
  UNUSED(argv);
  UNUSED(flags);
  unsigned char *value;
  unsigned int vlen;
  unsigned char *zm;
  zm = zipmapNew();
  zm =
      zipmapSet(zm, (unsigned char *)"foo", 3, (unsigned char *)"bar", 3, NULL);
  zm = zipmapSet(zm, (unsigned char *)"foo", 3, (unsigned char *)"!", 1, NULL);
  zm = zipmapSet(zm, (unsigned char *)"foo", 3, (unsigned char *)"12345", 5,
                 NULL);

  TEST_ASSERT(zipmapGet(zm, (unsigned char *)"foo", 3, &value, &vlen) == 1);
  zfree(zm);
  return 0;
}

int test_zipmapIterateThroughElements(int argc, char **argv, int flags) {
  UNUSED(argc);
  UNUSED(argv);
  UNUSED(flags);
  unsigned char *zm;
  zm = zipmapNew();
  unsigned char *i = zipmapRewind(zm);
  unsigned char *key, *value;
  unsigned int klen, vlen;

  zm = zipmapSet(zm, (unsigned char *)"name", 4, (unsigned char *)"foo", 3,
                 NULL);
  zm = zipmapSet(zm, (unsigned char *)"surname", 7, (unsigned char *)"foo", 3,
                 NULL);
  zm =
      zipmapSet(zm, (unsigned char *)"age", 3, (unsigned char *)"foo", 3, NULL);

  zm = zipmapSet(zm, (unsigned char *)"hello", 5, (unsigned char *)"world!", 6,
                 NULL);
  zm =
      zipmapSet(zm, (unsigned char *)"foo", 3, (unsigned char *)"bar", 3, NULL);
  zm = zipmapSet(zm, (unsigned char *)"foo", 3, (unsigned char *)"!", 1, NULL);
  zm = zipmapSet(zm, (unsigned char *)"foo", 3, (unsigned char *)"12345", 5,
                 NULL);
  zm = zipmapSet(zm, (unsigned char *)"new", 3, (unsigned char *)"xx", 2, NULL);
  zm = zipmapSet(zm, (unsigned char *)"noval", 5, (unsigned char *)"", 0, NULL);
  zm = zipmapDel(zm, (unsigned char *)"new", 3, NULL);

  while ((i = zipmapNext(i, &key, &klen, &value, &vlen)) != NULL) {
    TEST_ASSERT(klen > 0);
    TEST_ASSERT(key != NULL);
    TEST_ASSERT(vlen > 0);
    TEST_ASSERT(value != NULL);
  }
  zfree(zm);
  return 0;
}
