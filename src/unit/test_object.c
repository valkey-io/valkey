#include "../object.c"
#include "test_help.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <math.h>


int test_object_with_key(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    sds key = sdsnew("foo");
    robj *val = createStringObject("bar", strlen("bar"));
    TEST_ASSERT(val->encoding == OBJ_ENCODING_EMBSTR);

    /* Prevent objectSetKeyAndExpire from freeing the old val when reallocating it. */
    incrRefCount(val);

    /* Create valkey: val with key. */
    robj *valkey = objectSetKeyAndExpire(val, key, -1);
    TEST_ASSERT(valkey->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(objectGetKey(valkey) != NULL);

    /* Check embedded key "foo" */
    TEST_ASSERT(sdslen(objectGetKey(valkey)) == 3);
    TEST_ASSERT(sdslen(key) == 3);
    TEST_ASSERT(sdscmp(objectGetKey(valkey), key) == 0);
    TEST_ASSERT(strcmp(objectGetKey(valkey), "foo") == 0);

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

int test_embedded_string_with_key(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    int is_32bit = sizeof(void *) == 4;

    /* key of length 32 - type 8 */
    sds key = sdsnew("k:123456789012345678901234567890");

    /* value of length 7 should be embedded (length 11 for 32-bit) */
    const char *short_value = is_32bit ? "v:123456789" : "v:12345";
    robj *short_val_obj = createStringObject(short_value, strlen(short_value));

    /* value of length 8 should be raw (12 for 32-bit), because the total size
     * is over 64. */
    const char *longer_value = is_32bit ? "v:1234567890" : "v:123456";
    robj *longer_val_obj = createStringObject(longer_value, strlen(longer_value));

    robj *embstr_obj = objectSetKeyAndExpire(short_val_obj, key, -1);
    TEST_ASSERT(embstr_obj->encoding == OBJ_ENCODING_EMBSTR);

    robj *raw_obj = objectSetKeyAndExpire(longer_val_obj, key, -1);
    TEST_ASSERT(raw_obj->encoding == OBJ_ENCODING_RAW);

    sdsfree(key);
    decrRefCount(embstr_obj);
    decrRefCount(raw_obj);
    return 0;
}
