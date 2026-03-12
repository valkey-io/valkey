/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "server.h"
}

/* Metadata test helpers */
typedef struct objMetadata {
    uint32_t meta_int;
} objMetadata;

class ObjectTest : public ::testing::Test {
  protected:
    robj *createKeyValueObject(const char *k, const char *v) {
        sds key = sdsnew(k);
        robj *obj = createStringObject(v, strlen(v));
        robj *obj_with_key = objectSetKeyAndExpire(obj, key, -1);
        sdsfree(key);
        return obj_with_key;
    }

    void objectSetMetaInt(robj *o, uint32_t metadata_int) {
        objMetadata *meta = (objMetadata *)objectGetMetadata(o);
        meta->meta_int = metadata_int;
    }

    uint32_t objectGetMetaInt(const robj *o) {
        objMetadata *meta = (objMetadata *)objectGetMetadata(o);
        return meta->meta_int;
    }

    /* Find the largest value length that still embeds with the given key and expire. */
    int findMaxEmbeddableValueLen(const char *key, long long expire) {
        sds k = key ? sdsnew(key) : NULL;

        int len;
        for (len = 1; len <= 256; len++) {
            robj *obj = createStringObject(NULL, len);
            if (k) obj = objectSetKeyAndExpire(obj, k, expire);
            bool isEmbedded = (obj->encoding == OBJ_ENCODING_EMBSTR);
            decrRefCount(obj);
            if (!isEmbedded) break;
        }

        sdsfree(k);
        return len - 1;
    }
};

TEST_F(ObjectTest, object_with_key) {
    sds key = sdsnew("foo");
    robj *val = createStringObject("bar", strlen("bar"));
    ASSERT_EQ(val->encoding, (unsigned)OBJ_ENCODING_EMBSTR);
    ASSERT_EQ(sdslen((sds)objectGetVal(val)), 3u);

    /* Prevent objectSetKeyAndExpire from freeing the old val when reallocating it. */
    incrRefCount(val);

    robj *o = objectSetKeyAndExpire(val, key, -1);
    ASSERT_EQ(o->encoding, (unsigned)OBJ_ENCODING_EMBSTR);
    ASSERT_NE(objectGetKey(o), nullptr);

    /* Check embedded key "foo" */
    ASSERT_EQ(sdslen(objectGetKey(o)), 3u);
    ASSERT_EQ(sdslen(key), 3u);
    ASSERT_EQ(sdscmp(objectGetKey(o), key), 0);
    ASSERT_EQ(strcmp(objectGetKey(o), "foo"), 0);

    /* Check embedded value "bar" (EMBSTR content) */
    ASSERT_EQ(sdscmp((sds)objectGetVal(o), (sds)objectGetVal(val)), 0);
    ASSERT_EQ(strcmp((const char *)objectGetVal(o), "bar"), 0);
    ASSERT_EQ(sdslen((sds)objectGetVal(o)), 3u);

    /* Either they're two separate objects, or one object with refcount == 2. */
    if (o == val) {
        ASSERT_EQ((unsigned)o->refcount, 2u);
    } else {
        ASSERT_EQ((unsigned)o->refcount, 1u);
        ASSERT_EQ((unsigned)val->refcount, 1u);
    }

    /* Free them. */
    sdsfree(key);
    decrRefCount(val);
    decrRefCount(o);
}

TEST_F(ObjectTest, embedded_string_with_key) {
    const char *key = "k:123456789012345678901234567890";
    int max_len = findMaxEmbeddableValueLen(key, -1);
    ASSERT_GT(max_len, 0);

    /* Value at max length should embed. */
    sds k1 = sdsnew(key);
    robj *embstr_obj = createStringObject(NULL, max_len);
    embstr_obj = objectSetKeyAndExpire(embstr_obj, k1, -1);
    ASSERT_EQ(embstr_obj->encoding, (unsigned)OBJ_ENCODING_EMBSTR);
    ASSERT_EQ(sdslen((sds)objectGetVal(embstr_obj)), (size_t)max_len);

    /* One byte more should not embed. */
    sds k2 = sdsnew(key);
    robj *raw_obj = createStringObject(NULL, max_len + 1);
    raw_obj = objectSetKeyAndExpire(raw_obj, k2, -1);
    ASSERT_EQ(raw_obj->encoding, (unsigned)OBJ_ENCODING_RAW);
    ASSERT_EQ(sdslen((sds)objectGetVal(raw_obj)), (size_t)(max_len + 1));

    sdsfree(k1);
    sdsfree(k2);
    decrRefCount(embstr_obj);
    decrRefCount(raw_obj);
}

TEST_F(ObjectTest, embedded_string_with_key_and_expire) {
    const char *key = "k:123456789012345678901234567890";
    int max_len = findMaxEmbeddableValueLen(key, 128);
    ASSERT_GT(max_len, 0);

    /* Adding an expire reduces the available space for the value. */
    int max_len_no_expire = findMaxEmbeddableValueLen(key, -1);
    ASSERT_LT(max_len, max_len_no_expire);

    /* Value at max length should embed. */
    sds k1 = sdsnew(key);
    robj *embstr_obj = createStringObject(NULL, max_len);
    embstr_obj = objectSetKeyAndExpire(embstr_obj, k1, 128);
    ASSERT_EQ(embstr_obj->encoding, (unsigned)OBJ_ENCODING_EMBSTR);

    /* One byte more should not embed. */
    sds k2 = sdsnew(key);
    robj *raw_obj = createStringObject(NULL, max_len + 1);
    raw_obj = objectSetKeyAndExpire(raw_obj, k2, 128);
    ASSERT_EQ(raw_obj->encoding, (unsigned)OBJ_ENCODING_RAW);

    sdsfree(k1);
    sdsfree(k2);
    decrRefCount(embstr_obj);
    decrRefCount(raw_obj);
}

TEST_F(ObjectTest, embedded_value) {
    /* Value-only object (no key): find the largest value that embeds. */
    int max_len = findMaxEmbeddableValueLen(NULL, -1);
    ASSERT_GT(max_len, 0);

    robj *embstr_obj = createStringObject(NULL, max_len);
    ASSERT_EQ(embstr_obj->encoding, (unsigned)OBJ_ENCODING_EMBSTR);
    ASSERT_EQ(sdslen((sds)objectGetVal(embstr_obj)), (size_t)max_len);

    robj *raw_obj = createStringObject(NULL, max_len + 1);
    ASSERT_EQ(raw_obj->encoding, (unsigned)OBJ_ENCODING_RAW);

    decrRefCount(embstr_obj);
    decrRefCount(raw_obj);
}

TEST_F(ObjectTest, unembed_value) {
    const char *short_value = "embedded value";
    robj *short_val_obj = createStringObject(short_value, strlen(short_value));
    sds key = sdsnew("embedded key");
    long long expire = 155;

    robj *obj = objectSetKeyAndExpire(short_val_obj, key, expire);
    ASSERT_EQ(obj->encoding, (unsigned)OBJ_ENCODING_EMBSTR);
    ASSERT_EQ(strcmp((const char *)objectGetVal(obj), short_value), 0);
    ASSERT_EQ(sdscmp(objectGetKey(obj), key), 0);
    ASSERT_EQ(objectGetExpire(obj), expire);
    ASSERT_NE(objectGetVal(obj), short_value);

    /* Unembed the value - it uses a separate allocation now.
     * the other embedded data gets shifted, so check them too */
    objectUnembedVal(obj);
    ASSERT_EQ(obj->encoding, (unsigned)OBJ_ENCODING_RAW);
    ASSERT_EQ(strcmp((const char *)objectGetVal(obj), short_value), 0);
    ASSERT_EQ(sdscmp(objectGetKey(obj), key), 0);
    ASSERT_EQ(objectGetExpire(obj), expire);
    ASSERT_NE(objectGetVal(obj), short_value); /* different allocation, different copy */

    sdsfree(key);
    decrRefCount(obj);
}


TEST_F(ObjectTest, metadata_disabled) {
    robj *obj_with_key = createKeyValueObject("testkey", "value");

    ASSERT_EQ(objectGetMetadata(obj_with_key), nullptr);
    ASSERT_EQ(objectGetMetadataSize(obj_with_key), 0u);

    decrRefCount(obj_with_key);
}

TEST_F(ObjectTest, metadata_without_key) {
    objectSetMetadataSize(sizeof(objMetadata));

    robj *obj_no_key = createStringObject("value_without_key", 17);

    ASSERT_EQ(objectGetMetadata(obj_no_key), nullptr);
    ASSERT_EQ(objectGetMetadataSize(obj_no_key), 0u);

    decrRefCount(obj_no_key);
}

TEST_F(ObjectTest, metadata_with_key) {
    objectSetMetadataSize(sizeof(objMetadata));

    robj *obj_with_key = createKeyValueObject("testkey", "value");

    ASSERT_EQ(objectGetMetadataSize(obj_with_key), sizeof(objMetadata));

    objMetadata *meta = (objMetadata *)objectGetMetadata(obj_with_key);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->meta_int, 0u);

    decrRefCount(obj_with_key);
}

TEST_F(ObjectTest, metadata_read_write) {
    objectSetMetadataSize(sizeof(objMetadata));

    robj *obj_with_key = createKeyValueObject("mykey", "myvalue");

    ASSERT_EQ(objectGetMetadataSize(obj_with_key), sizeof(objMetadata));

    objectSetMetaInt(obj_with_key, 12345);
    EXPECT_EQ(objectGetMetaInt(obj_with_key), 12345u);

    objectSetMetaInt(obj_with_key, 67890);
    EXPECT_EQ(objectGetMetaInt(obj_with_key), 67890u);

    decrRefCount(obj_with_key);
}

TEST_F(ObjectTest, metadata_multiple_objects) {
    objectSetMetadataSize(sizeof(objMetadata));

    robj *obj_with_key1 = createKeyValueObject("key1", "val1");
    robj *obj_with_key2 = createKeyValueObject("key2", "val2");
    robj *obj_with_key3 = createKeyValueObject("key3", "val3");

    ASSERT_EQ(objectGetMetadataSize(obj_with_key1), sizeof(objMetadata));
    ASSERT_EQ(objectGetMetadataSize(obj_with_key2), sizeof(objMetadata));
    ASSERT_EQ(objectGetMetadataSize(obj_with_key3), sizeof(objMetadata));

    objectSetMetaInt(obj_with_key1, 100);
    objectSetMetaInt(obj_with_key2, 200);
    objectSetMetaInt(obj_with_key3, 300);

    EXPECT_EQ(objectGetMetaInt(obj_with_key1), 100u);
    EXPECT_EQ(objectGetMetaInt(obj_with_key2), 200u);
    EXPECT_EQ(objectGetMetaInt(obj_with_key3), 300u);

    objectSetMetaInt(obj_with_key2, 999);
    EXPECT_EQ(objectGetMetaInt(obj_with_key1), 100u);
    EXPECT_EQ(objectGetMetaInt(obj_with_key2), 999u);
    EXPECT_EQ(objectGetMetaInt(obj_with_key3), 300u);

    decrRefCount(obj_with_key1);
    decrRefCount(obj_with_key2);
    decrRefCount(obj_with_key3);
}

TEST_F(ObjectTest, metadata_changes_embed_threshold) {
    /* Find the max embeddable value length without metadata, then verify
     * that enabling metadata reduces it (some previously-embeddable objects
     * become RAW). */
    const char *key = "k:123456789012345678901234567890";
    int max_without = findMaxEmbeddableValueLen(key, -1);
    ASSERT_GT(max_without, 0);

    objectSetMetadataSize(sizeof(objMetadata));
    int max_with = findMaxEmbeddableValueLen(key, -1);

    /* Metadata takes space, so the threshold must shrink. */
    ASSERT_LT(max_with, max_without);

    /* An object that just fit before should now be RAW. */
    sds k = sdsnew(key);
    robj *obj = createStringObject(NULL, max_without);
    obj = objectSetKeyAndExpire(obj, k, -1);
    ASSERT_EQ(obj->encoding, (unsigned)OBJ_ENCODING_RAW);

    sdsfree(k);
    decrRefCount(obj);
}
