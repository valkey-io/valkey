#include "../object.c"
#include "test_help.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <math.h>


int test_valkey_from_embstr(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    sds key = sdsnew("foo");
    robj *val = createStringObject("bar", strlen("bar"));
    TEST_ASSERT(val->encoding == OBJ_ENCODING_EMBSTR);

    /* Prevent objectConvertToValkey from freeing val when converting it. */
    incrRefCount(val);

    /* Create valkey: val with key. */
    valkey *valkey = objectConvertToValkey(val, key);
    TEST_ASSERT(valkey->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(valkeyGetKey(valkey) != NULL);

    /* Check embedded key "foo" */
    TEST_ASSERT(sdslen(valkeyGetKey(valkey)) == 3);
    TEST_ASSERT(sdslen(key) == 3);
    TEST_ASSERT(sdscmp(valkeyGetKey(valkey), key) == 0);
    TEST_ASSERT(strcmp(valkeyGetKey(valkey), "foo") == 0);

    /* Check embedded value "bar" (EMBSTR content) */
    TEST_ASSERT(sdscmp(valkey->ptr, val->ptr) == 0);
    TEST_ASSERT(strcmp(valkey->ptr, "bar") == 0);

    /* Either they're two separate objects, or one object with refcount == 2. */
    if (valkey == val) {
        TEST_ASSERT(valkey->refcount == 2);
    } else {
        TEST_ASSERT(valkey->refcount == 1);
        TEST_ASSERT(val->refcount == 1);
    }

    /* Free them. */
    sdsfree(key);
    decrRefCount(val);
    decrRefCount(valkey);
    return 0;
}
