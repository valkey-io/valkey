#include "../object.c"
#include "test_help.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <math.h>


int test_object_with_key(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    sds key = sdsnew("foo");
    robj *val = createStringObject("bar", strlen("bar"));
    TEST_ASSERT(val->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(sdslen(objectGetVal(val)) == 3);

    /* Prevent objectSetKeyAndExpire from freeing the old val when reallocating it. */
    incrRefCount(val);

    robj *o = objectSetKeyAndExpire(val, key, -1);
    TEST_ASSERT(o->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(objectGetKey(o) != NULL);

    /* Check embedded key "foo" */
    TEST_ASSERT(sdslen(objectGetKey(o)) == 3);
    TEST_ASSERT(sdslen(key) == 3);
    TEST_ASSERT(sdscmp(objectGetKey(o), key) == 0);
    TEST_ASSERT(strcmp(objectGetKey(o), "foo") == 0);

    /* Check embedded value "bar" (EMBSTR content) */
    TEST_ASSERT(sdscmp(objectGetVal(o), objectGetVal(val)) == 0);
    TEST_ASSERT(strcmp(objectGetVal(o), "bar") == 0);
    TEST_ASSERT(sdslen(objectGetVal(o)) == 3);

    /* Either they're two separate objects, or one object with refcount == 2. */
    if (o == val) {
        TEST_ASSERT(o->refcount == 2);
    } else {
        TEST_ASSERT(o->refcount == 1);
        TEST_ASSERT(val->refcount == 1);
    }

    /* Free them. */
    sdsfree(key);
    decrRefCount(val);
    decrRefCount(o);
    return 0;
}

int test_embedded_string_with_key(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* key of length 32 - type 8 */
    sds key = sdsnew("k:123456789012345678901234567890");
    TEST_ASSERT(sdslen(key) == 32);

    /* 32B key and 15B value should be embedded within 64B. Contents:
     * - 8B robj (no ptr) + 1B key header size
     * - 3B key header + 32B key + 1B null terminator
     * - 3B val header + 15B val + 1B null terminator
     * because no pointers are stored, there is no difference for 32 bit builds*/
    const char *short_value = "123456789012345";
    TEST_ASSERT(strlen(short_value) == 15);
    robj *short_val_obj = createStringObject(short_value, strlen(short_value));
    robj *embstr_obj = objectSetKeyAndExpire(short_val_obj, key, -1);
    TEST_ASSERT(embstr_obj->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(sdslen(objectGetKey(embstr_obj)) == 32);
    TEST_ASSERT(sdscmp(objectGetKey(embstr_obj), key) == 0);
    TEST_ASSERT(sdslen(objectGetVal(embstr_obj)) == 15);
    TEST_ASSERT(strcmp(objectGetVal(embstr_obj), short_value) == 0);

    /* value of length 16 cannot be embedded with other contents within 64B */
    const char *longer_value = "1234567890123456";
    TEST_ASSERT(strlen(longer_value) == 16);
    robj *longer_val_obj = createStringObject(longer_value, strlen(longer_value));
    robj *raw_obj = objectSetKeyAndExpire(longer_val_obj, key, -1);
    TEST_ASSERT(raw_obj->encoding == OBJ_ENCODING_RAW);
    TEST_ASSERT(sdslen(objectGetKey(raw_obj)) == 32);
    TEST_ASSERT(sdscmp(objectGetKey(raw_obj), key) == 0);
    TEST_ASSERT(sdslen(objectGetVal(raw_obj)) == 16);
    TEST_ASSERT(strcmp(objectGetVal(raw_obj), longer_value) == 0);

    sdsfree(key);
    decrRefCount(embstr_obj);
    decrRefCount(raw_obj);
    return 0;
}

int test_embedded_string_with_key_and_expire(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* key of length 32 - type 8 */
    sds key = sdsnew("k:123456789012345678901234567890");
    TEST_ASSERT(sdslen(key) == 32);

    /* 32B key and 7B value should be embedded within 64B. Contents:
     * - 8B robj (no ptr) + 8B expire + 1B key header size
     * - 3B key header + 32B key + 1B null terminator
     * - 3B val header + 7B val + 1B null terminator
     * because no pointers are stored, there is no difference for 32 bit builds*/
    const char *short_value = "1234567";
    TEST_ASSERT(strlen(short_value) == 7);
    robj *short_val_obj = createStringObject(short_value, strlen(short_value));
    robj *embstr_obj = objectSetKeyAndExpire(short_val_obj, key, 128);
    TEST_ASSERT(embstr_obj->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(sdslen(objectGetKey(embstr_obj)) == 32);
    TEST_ASSERT(sdscmp(objectGetKey(embstr_obj), key) == 0);
    TEST_ASSERT(sdslen(objectGetVal(embstr_obj)) == 7);
    TEST_ASSERT(strcmp(objectGetVal(embstr_obj), short_value) == 0);

    /* value of length 8 cannot be embedded with other contents within 64B */
    const char *longer_value = "12345678";
    TEST_ASSERT(strlen(longer_value) == 8);
    robj *longer_val_obj = createStringObject(longer_value, strlen(longer_value));
    robj *raw_obj = objectSetKeyAndExpire(longer_val_obj, key, 128);
    TEST_ASSERT(raw_obj->encoding == OBJ_ENCODING_RAW);
    TEST_ASSERT(sdslen(objectGetKey(raw_obj)) == 32);
    TEST_ASSERT(sdscmp(objectGetKey(raw_obj), key) == 0);
    TEST_ASSERT(sdslen(objectGetVal(raw_obj)) == 8);
    TEST_ASSERT(strcmp(objectGetVal(raw_obj), longer_value) == 0);

    sdsfree(key);
    decrRefCount(embstr_obj);
    decrRefCount(raw_obj);
    return 0;
}

int test_embedded_value(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* with only value there is only 12B overhead, so we can embed up to 52B.
     * 8B robj (no ptr) + 3B val header + 52B val + 1B null terminator */
    const char *val = "v:12345678901234567890123456789012345678901234567890";
    TEST_ASSERT(strlen(val) == 52);
    robj *embstr_obj = createStringObject(val, strlen(val));
    TEST_ASSERT(embstr_obj->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(sdslen(objectGetVal(embstr_obj)) == 52);
    TEST_ASSERT(strcmp(objectGetVal(embstr_obj), val) == 0);

    decrRefCount(embstr_obj);
    return 0;
}

int test_unembed_value(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *short_value = "embedded value";
    robj *short_val_obj = createStringObject(short_value, strlen(short_value));
    sds key = sdsnew("embedded key");
    long long expire = 155;

    robj *obj = objectSetKeyAndExpire(short_val_obj, key, expire);
    TEST_ASSERT(obj->encoding == OBJ_ENCODING_EMBSTR);
    TEST_ASSERT(strcmp(objectGetVal(obj), short_value) == 0);
    TEST_ASSERT(sdscmp(objectGetKey(obj), key) == 0);
    TEST_ASSERT(objectGetExpire(obj) == expire);
    TEST_ASSERT(objectGetVal(obj) != short_value);

    /* Unembed the value - it uses a separate allocation now.
     * the other embedded data gets shifted, so check them too */
    objectUnembedVal(obj);
    TEST_ASSERT(obj->encoding == OBJ_ENCODING_RAW);
    TEST_ASSERT(strcmp(objectGetVal(obj), short_value) == 0);
    TEST_ASSERT(sdscmp(objectGetKey(obj), key) == 0);
    TEST_ASSERT(objectGetExpire(obj) == expire);
    TEST_ASSERT(objectGetVal(obj) != short_value); /* different allocation, different copy */

    sdsfree(key);
    decrRefCount(obj);
    return 0;
}
