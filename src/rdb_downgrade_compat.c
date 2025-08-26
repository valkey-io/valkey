/*
 * RDB Downgrade Compatibility Implementation
 *
 * This module provides compatibility functions to enable Redis 6.0
 * to load RDB files from newer versions (Redis 7.x RDB v10 and Valkey 8.x RDB v11)
 * that use listpack encoding instead of ziplist encoding.
 *
 */

#include "rdb_downgrade_compat.h"
#include "server.h"
#include "rdb.h"
#include "listpack.h"
#include "quicklist.h"
#include "ziplist.h"
#include "sds.h"
#include "endianconv.h"
#include <time.h>

/* Listpack format constants for Redis 6.2 compatibility */
#define LP_HDR_SIZE                                                            \
  6 /* Listpack header size: 4 bytes total + 2 bytes num-elements */
#define LP_EOF 0xFF /* Listpack EOF marker */

/* When rdbLoadObject() returns NULL, the err flag is
 * set to hold the type of error that occurred */
#define RDB_LOAD_ERR_EMPTY_KEY 1 /* Error of empty key */
#define RDB_LOAD_ERR_OTHER 2     /* Any other errors */

/* Downgrade monitoring statistics */
struct rdbDowngradeStats rdb_downgrade_stats = {0};

/* Get data structure type as a string */
const char *getDataStructureType(int rdbtype) {
    switch (rdbtype) {
        case RDB_TYPE_HASH_ZIPLIST:
        case RDB_TYPE_HASH_LISTPACK:
            return "hash";
        case RDB_TYPE_LIST_ZIPLIST:
        case RDB_TYPE_LIST_QUICKLIST_2:
            return "list";
        case RDB_TYPE_SET_LISTPACK:
            return "set";
        case RDB_TYPE_ZSET_ZIPLIST:
        case RDB_TYPE_ZSET_LISTPACK:
            return "sortedset";
        default:
            return "unknown";
    }
}

/* Update downgrade statistics with enhanced per-data-structure tracking */
void rdbDowngradeStatsUpdateWithType(int success, size_t bytes, const char *key, int rdbtype) {
    rdb_downgrade_stats.keys_attempted++;
    
    
    if (success) {
        rdb_downgrade_stats.keys_succeeded++;
        rdb_downgrade_stats.bytes_converted += bytes;
        rdb_downgrade_stats.keys_converted++;
        serverLog(LL_VERBOSE, "RDB downgrade success: key=%s, type=%s, bytes=%zu, total_keys=%llu",
                  key ? key : "unknown", getDataStructureType(rdbtype), bytes, rdb_downgrade_stats.keys_converted);
    } else {
        rdb_downgrade_stats.keys_failed++;
        serverLog(LL_WARNING, "RDB downgrade failed: key=%s, type=%s, total_failures=%llu",
                  key ? key : "unknown", getDataStructureType(rdbtype), rdb_downgrade_stats.keys_failed);
    }
    rdb_downgrade_stats.last_conversion_time = time(NULL);
}

/* Update downgrade statistics with version check to prevent false positives */
void rdbDowngradeStatsUpdateWithTypeAndVersion(int success, size_t bytes, const char *key, int rdbtype, int rdbver) {
    /* Only increment statistics for RDB versions that actually require downgrade conversion */
    if (rdbver < RDB_VERSION_REDIS_70) {
        serverLog(LL_DEBUG, "RDB compatibility: Skipping statistics update for RDB v%d key=%s (no conversion needed)", rdbver, key);
        return;
    }
    
    /* Call original function for versions that do require conversion */
    rdbDowngradeStatsUpdateWithType(success, bytes, key, rdbtype);
}



/* Generate statistics info string for INFO command */
sds rdbDowngradeStatsInfoString(void) {
    return sdscatprintf(sdsempty(),
        "# RDBDowngradeStats\r\n"
        "rdb_downgrade_keys_attempted:%llu\r\n"
        "rdb_downgrade_keys_succeeded:%llu\r\n"
        "rdb_downgrade_keys_failed:%llu\r\n"
        "rdb_downgrade_bytes_converted:%llu\r\n"
        "rdb_downgrade_last_conversion_time:%ld\r\n",
        rdb_downgrade_stats.keys_attempted,
        rdb_downgrade_stats.keys_succeeded,
        rdb_downgrade_stats.keys_failed,
        rdb_downgrade_stats.bytes_converted,
        rdb_downgrade_stats.last_conversion_time);
}

/* Check if this RDB type requires listpack to ziplist conversion */
int requiresListpackConversion(int rdbtype, int rdbver) {
    /* For RDB versions below 10, no conversion is needed */
    if (rdbver < RDB_VERSION_REDIS_70) return 0;

    switch (rdbtype) {
        /* These types contain listpack data and need conversion to ziplist */
        case RDB_TYPE_HASH_LISTPACK:
        case RDB_TYPE_ZSET_LISTPACK:
        case RDB_TYPE_LIST_QUICKLIST_2:
        case RDB_TYPE_STREAM_LISTPACKS_2:
        case RDB_TYPE_SET_LISTPACK:
        case RDB_TYPE_STREAM_LISTPACKS_3:
            return 1;
        default:
            return 0;
    }
}

/* Enhanced listpack detection specifically for RDB v11 compatibility */
int isListpackEncoded(unsigned char *data, size_t len) {
    if (!data || len < LISTPACK_MIN_VALID_SIZE || len > MAX_LISTPACK_SIZE) {
        return 0;
    }
    
    /* For RDB v11 compatibility, we need to be more aggressive in detecting listpacks
     * since Valkey consistently uses listpack format for RDB v11 */
    
    /* Basic listpack header check */
    if (len < 6) return 0; /* Need at least 6 bytes for header */
    
    uint32_t total_bytes;
    uint16_t num_elements;
    
    /* Safe header extraction */
    memcpy(&total_bytes, data, 4);
    memcpy(&num_elements, data + 4, 2);
    
    /* Convert from little endian if needed */
    total_bytes = intrev32ifbe(total_bytes);
    num_elements = intrev16ifbe(num_elements);
    
    /* First check: does it look like a valid listpack? */
    if (total_bytes == len &&
        total_bytes >= LISTPACK_MIN_VALID_SIZE &&
        total_bytes <= MAX_LISTPACK_SIZE &&
        len > 0 && data[len-1] == LP_EOF) {
        
        /* This looks like a valid listpack */
        return 1;
    }
    
    /* Second check: does it look like a ziplist? */
    if (len >= 10) {
        uint32_t zl_bytes, zl_tail_offset;
        uint16_t zl_length;
        
        memcpy(&zl_bytes, data, 4);
        memcpy(&zl_tail_offset, data + 4, 4);
        memcpy(&zl_length, data + 8, 2);
        
        zl_bytes = intrev32ifbe(zl_bytes);
        zl_tail_offset = intrev32ifbe(zl_tail_offset);
        zl_length = intrev16ifbe(zl_length);
        
        /* If it looks like a valid ziplist, it's not a listpack */
        if (zl_bytes == len && zl_tail_offset < len &&
            len > 10 && data[len-1] == 0xFF) { /* ziplist EOF marker */
            return 0;
        }
    }
    
    /* For RDB v11, if it doesn't look like a ziplist and has basic listpack structure, assume listpack */
    if (total_bytes == len && num_elements < MAX_CONVERSION_ELEMENTS) {
        return 1;
    }
    
    return 0;
}


/* Convert listpack to ziplist format with comprehensive error handling */
unsigned char *listpackToZiplist(unsigned char *lp) {
    if (!lp) {
        serverLog(LL_WARNING, "RDB compatibility: listpackToZiplist called with NULL pointer");
        return NULL;
    }
    
    /* Basic safety checks before calling any listpack functions */
    if ((unsigned char *)lp < (unsigned char *)0x1000) {
        serverLog(LL_WARNING, "listpackToZiplist: invalid listpack pointer");
        return NULL;
    }
    
    /* Get listpack size safely */
    size_t lp_size = lpBytes(lp);
    if (lp_size > MAX_LISTPACK_SIZE || lp_size < LISTPACK_MIN_VALID_SIZE) {
        serverLog(LL_WARNING, "listpackToZiplist: invalid listpack size (%zu bytes)", lp_size);
        return NULL;
    }
    
    /* Get element count safely */
    uint32_t num_elements = lpLength(lp);
    if (num_elements > MAX_CONVERSION_ELEMENTS) {
        serverLog(LL_WARNING, "listpackToZiplist: too many elements (%u)", num_elements);
        return NULL;
    }
    
    /* Create new ziplist */
    unsigned char *zl = ziplistNew();
    if (!zl) {
        serverLog(LL_WARNING, "listpackToZiplist: failed to create ziplist");
        return NULL;
    }
    
    /* If empty listpack, return empty ziplist */
    if (num_elements == 0) {
        return zl;
    }
    
    /* Get first element safely */
    unsigned char *p = lpFirst(lp);
    if (!p) {
        serverLog(LL_WARNING, "listpackToZiplist: failed to get first element");
        zfree(zl);
        return NULL;
    }
    
    uint32_t processed = 0;
    
    while (p && processed < num_elements) {
        int64_t slen;
        unsigned char intbuf[32];
        unsigned char *sval;
        unsigned char *new_zl;
        
        /* Bounds check */
        if (p < lp || p >= lp + lp_size - 1) {
            serverLog(LL_WARNING, "listpackToZiplist: element pointer out of bounds");
            goto error_cleanup;
        }
        
        /* Get element value safely */
        sval = lpGet(p, &slen, intbuf);
        
        if (sval) {
            /* String value - validate size */
            if (slen > (int64_t)server.proto_max_bulk_len) {
                serverLog(LL_WARNING, "listpackToZiplist: string element too large (%lld bytes)", (long long)slen);
                goto error_cleanup;
            }
            
            new_zl = ziplistPush(zl, sval, (size_t)slen, ZIPLIST_TAIL);
        } else {
            /* Integer value - use the buffer directly */
            new_zl = ziplistPush(zl, intbuf, (size_t)slen, ZIPLIST_TAIL);
        }
        
        if (!new_zl) {
            serverLog(LL_WARNING, "listpackToZiplist: failed to add element %u", processed);
            goto error_cleanup;
        }
        
        zl = new_zl;
        processed++;
        
        /* Get next element safely */
        unsigned char *next_p = lpNext(lp, p);
        if (!next_p && processed < num_elements) {
            serverLog(LL_WARNING, "listpackToZiplist: unexpected end of listpack at element %u", processed);
            goto error_cleanup;
        }
        
        p = next_p;
        
        /* Safety check for infinite loops */
        if (processed >= MAX_CONVERSION_ELEMENTS) {
            serverLog(LL_WARNING, "listpackToZiplist: conversion element limit exceeded");
            goto error_cleanup;
        }
    }
    
    /* Verify we processed all elements */
    if (processed != num_elements) {
        serverLog(LL_WARNING, "listpackToZiplist: element count mismatch (expected %u, got %u)",
                  num_elements, processed);
        goto error_cleanup;
    }
    
    /* Final ziplist validation */
    if (ziplistLen(zl) != num_elements) {
        serverLog(LL_WARNING, "listpackToZiplist: final ziplist length mismatch");
        goto error_cleanup;
    }
    
    return zl;
    
error_cleanup:
    if (zl) {
        zfree(zl);
    }
    return NULL;
}

/* Enhanced object loading with compatibility for higher RDB versions */
robj *rdbLoadObjectCompat(int rdbtype, rio *rdb, sds key, int *error, int rdbver) {
    /* Performance and error tracking */
    mstime_t start_time = mstime();
    int conversion_attempted = 0;
    size_t original_size = 0, converted_size = 0;
    
    /* Validate inputs to prevent corruption */
    if (!rdb || !key) {
        serverLog(LL_WARNING, "RDB compatibility: Invalid arguments - rdb=%p, key=%s", (void*)rdb, key);
        if (error) *error = RDB_LOAD_ERR_OTHER;
        return NULL;
    }
    
    /* For RDB versions below 10, use original loading logic */
    if (rdbver < RDB_VERSION_REDIS_70) {
        serverLog(LL_DEBUG, "RDB compatibility: Using standard loader for RDB v%d key=%s", rdbver, key);
        return rdbLoadObject(rdbtype, rdb, key);
    }
    
    /* Handle RDB v11 stream type 21 (RDB_TYPE_STREAM_LISTPACKS_3) with active_time support */
    if (rdbtype == RDB_TYPE_STREAM_LISTPACKS_3) {
        serverLog(LL_VERBOSE, "RDB compatibility: Processing RDB_TYPE_STREAM_LISTPACKS_3 (type 21) for key=%s", key);
        
        /* Load stream as RDB_TYPE_STREAM_LISTPACKS but handle extra active_time fields */
        return rdbLoadStreamWithActiveTime(rdb, key, error, rdbver);
    }
    
    /* Initialize variables */
    robj *o = NULL;
    unsigned char *encoded = NULL;
    size_t encoded_len = 0;
    
    /* Initialize error state */
    if (error) *error = RDB_LOAD_ERR_OTHER;
    
    /* Log compatibility layer activation */
    serverLog(LL_VERBOSE, "RDB compatibility: Processing RDB v%d key=%s type=%s", rdbver, key, getDataStructureType(rdbtype));
    
    /* Handle compatibility conversion for supported types */
    if (requiresListpackConversion(rdbtype, rdbver) != 0) {
        
        /* Special handling for LIST_QUICKLIST_2 which has different structure */
        if (rdbtype == RDB_TYPE_LIST_QUICKLIST_2) {
            serverLog(LL_VERBOSE, "RDB compatibility: Processing LIST_QUICKLIST_2 for key=%s, type=%s (RDB v%d)", key, getDataStructureType(rdbtype), rdbver);
            
            uint64_t len;
            if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to load quicklist length for key=%s", key);
                goto error_cleanup;
            }
            if (len == 0) {
                if (error) *error = RDB_LOAD_ERR_EMPTY_KEY;
                return NULL;
            }

            o = createQuicklistObject();
            if (!o) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to create quicklist object for key=%s", key);
                goto error_cleanup;
            }
            
            quicklistSetOptions(o->ptr, server.list_max_ziplist_size, server.list_compress_depth);
            size_t total_original_size = 0;
            size_t total_converted_size = 0;

            while (len--) {
                uint64_t container;
                if ((container = rdbLoadLen(rdb, NULL)) == RDB_LENERR) {
                    serverLog(LL_WARNING, "RDB compatibility: Failed to load quicklist container type for key=%s", key);
                    decrRefCount(o);
                    goto error_cleanup;
                }

                if (container != QUICKLIST_NODE_CONTAINER_PACKED && container != QUICKLIST_NODE_CONTAINER_PLAIN) {
                    serverLog(LL_WARNING, "RDB compatibility: Invalid quicklist container type %llu for key=%s", (unsigned long long)container, key);
                    decrRefCount(o);
                    goto error_cleanup;
                }

                size_t node_encoded_len;
                unsigned char *node_encoded =
                    rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,&node_encoded_len);
                if (!node_encoded) {
                    serverLog(LL_WARNING, "RDB compatibility: Failed to load quicklist node for key=%s", key);
                    decrRefCount(o);
                    goto error_cleanup;
                }
                
                original_size = node_encoded_len;
                total_original_size += original_size;
                conversion_attempted = 1;

                unsigned char *zl = NULL;
                if (container == QUICKLIST_NODE_CONTAINER_PLAIN) {
                    zl = ziplistNew();
                    zl = ziplistPush(zl, node_encoded, node_encoded_len, ZIPLIST_TAIL);
                    zfree(node_encoded);
                    node_encoded = NULL;
                } else { /* QUICKLIST_NODE_CONTAINER_PACKED */
                    zl = ziplistNew();
                    if (!quicklistConvertAndValidateIntegrity(node_encoded, &zl)) {
                        serverLog(LL_WARNING, "RDB compatibility: listpack to ziplist conversion failed for quicklist node key=%s, skipping node", key);
                        zfree(node_encoded);
                        zfree(zl);
                        continue; /* Skip this node instead of failing completely */
                    }
                    zfree(node_encoded);
                    node_encoded = NULL;
                }

                if (ziplistLen(zl) > 0) {
                    total_converted_size += ziplistBlobLen(zl);
                    quicklistAppendZiplist(o->ptr, zl);
                } else {
                    zfree(zl);
                }
            }

            if (quicklistCount(o->ptr) == 0) {
                serverLog(LL_WARNING, "RDB compatibility: Created empty quicklist for key=%s", key);
            }
            
            if (conversion_attempted) {
                rdbDowngradeStatsUpdateWithTypeAndVersion(1, total_converted_size, key, rdbtype, rdbver);
                serverLog(LL_VERBOSE, "RDB compatibility: Successfully converted listpack to ziplist for key=%s, type=%s (RDB v%d, %zu->%zu bytes)",
                          key, getDataStructureType(rdbtype), rdbver, total_original_size, total_converted_size);
            }
            
            mstime_t elapsed = mstime() - start_time;
            serverLog(LL_VERBOSE, "RDB compatibility: Successfully processed LIST_QUICKLIST_2 key=%s, type=%s in %lldms", key, getDataStructureType(rdbtype), elapsed);
            
            if (error) *error = 0;
            return o;
        } else if (rdbtype == RDB_TYPE_HASH_ZIPLIST ||
                   rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
                   rdbtype == RDB_TYPE_LIST_ZIPLIST ||
                   rdbtype == RDB_TYPE_HASH_LISTPACK ||
                   rdbtype == RDB_TYPE_ZSET_LISTPACK ||
                   rdbtype == RDB_TYPE_SET_LISTPACK) {
            
            conversion_attempted = 1;
            serverLog(LL_DEBUG, "RDB compatibility: Attempting conversion for key=%s, type=%s", key, getDataStructureType(rdbtype));
            
            /* Load the raw data with comprehensive error handling */
            encoded = rdbGenericLoadStringObject(rdb, RDB_LOAD_PLAIN, &encoded_len);
            if (!encoded) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to load raw data for key=%s", key);
                goto error_cleanup;
            }
            
            original_size = encoded_len;
            serverLog(LL_DEBUG, "RDB compatibility: Loaded raw data size=%zu for key=%s", encoded_len, key);
            
            /* Comprehensive data validation */
            if (encoded_len == 0) {
                serverLog(LL_WARNING, "RDB compatibility: Empty data for key=%s", key);
                goto error_cleanup;
            }
            
            if (encoded_len > MAX_LISTPACK_SIZE) {
                serverLog(LL_WARNING, "RDB compatibility: Data too large (%zu bytes) for key=%s, max=%d",
                          encoded_len, key, MAX_LISTPACK_SIZE);
                goto error_cleanup;
            }
            
            /* Detect and convert listpack data */
            if (isListpackEncoded(encoded, encoded_len)) {
                serverLog(LL_VERBOSE, "RDB compatibility: Detected listpack encoding for key=%s, type=%s, converting...", key, getDataStructureType(rdbtype));
                
                /* Convert listpack to ziplist */
                unsigned char *zl = listpackToZiplist(encoded);
                if (!zl) {
                    serverLog(LL_WARNING, "RDB compatibility: Listpack to ziplist conversion failed for key=%s, type=%s", key, getDataStructureType(rdbtype));
                    goto error_cleanup;
                }
                
                /* Validate converted ziplist */
                size_t zl_len = ziplistBlobLen(zl);
                if (zl_len == 0 || zl_len > MAX_LISTPACK_SIZE) {
                    serverLog(LL_WARNING, "RDB compatibility: Invalid ziplist size (%zu) for key=%s", zl_len, key);
                    zfree(zl);
                    goto error_cleanup;
                }
                
                /* Successfully converted */
                zfree(encoded);
                encoded = zl;
                encoded_len = zl_len;
                converted_size = zl_len;
                
                serverLog(LL_VERBOSE, "RDB compatibility: Successfully converted listpack to ziplist for key=%s, type=%s (RDB v%d, %zu->%zu bytes)",
                          key, getDataStructureType(rdbtype), rdbver, original_size, converted_size);
            } else {
                /* Data is already in ziplist format */
                serverLog(LL_DEBUG, "RDB compatibility: Data already in ziplist format for key=%s, type=%s", key, getDataStructureType(rdbtype));
                converted_size = encoded_len;
            }
            
            /* Create object with comprehensive safety checks */
            if (!encoded || encoded_len == 0) {
                serverLog(LL_WARNING, "RDB compatibility: No data to create object for key=%s", key);
                goto error_cleanup;
            }
            
            /* Create object with the data - object takes ownership of encoded */
            o = createObject(OBJ_STRING, encoded);
            if (!o) {
                serverLog(LL_WARNING, "RDB compatibility: Object creation failed for key=%s", key);
                goto error_cleanup;
            }
            
            /* From this point, encoded is owned by the object */
            encoded = NULL; /* Prevent double-free */
            
            /* Set the correct object type and encoding with comprehensive validation */
            switch (rdbtype) {
                case RDB_TYPE_HASH_ZIPLIST:
                case RDB_TYPE_HASH_LISTPACK:
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_ZIPLIST;
                    
                    /* Validate hash structure integrity */
                    
                    /* Convert to hash table if too large for ziplist */
                    if (hashTypeLength(o) > server.hash_max_ziplist_entries) {
                        serverLog(LL_DEBUG, "RDB compatibility: Converting hash to HT encoding for key=%s (size=%lu)",
                                  key, hashTypeLength(o));
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                    break;
                    
                case RDB_TYPE_ZSET_ZIPLIST:
                case RDB_TYPE_ZSET_LISTPACK:
                    o->type = OBJ_ZSET;
                    o->encoding = OBJ_ENCODING_ZIPLIST;
                    
                    /* Validate sorted set structure integrity */
                    
                    /* Convert to skiplist if too large for ziplist */
                    if (zsetLength(o) > server.zset_max_ziplist_entries) {
                        serverLog(LL_DEBUG, "RDB compatibility: Converting zset to skiplist encoding for key=%s (size=%lu)",
                                  key, zsetLength(o));
                        zsetConvert(o, OBJ_ENCODING_SKIPLIST);
                    }
                    break;
                    
                case RDB_TYPE_LIST_ZIPLIST:
                    o->type = OBJ_LIST;
                    o->encoding = OBJ_ENCODING_ZIPLIST;
                    
                    /* Validate list structure integrity */
                    
                    /* Convert to quicklist as Redis 6.0 expects */
                    serverLog(LL_DEBUG, "RDB compatibility: Converting list to quicklist encoding for key=%s", key);
                    listTypeConvert(o, OBJ_ENCODING_QUICKLIST);
                    break;
                    
                case RDB_TYPE_SET_LISTPACK:
                    /* Set with listpack encoding - convert to regular set */
                    o->type = OBJ_SET;
                    o->encoding = OBJ_ENCODING_HT;
                    
                    /* Create hash table and populate from converted ziplist */
                    dict *set_dict = dictCreate(&setDictType, NULL);
                    if (!set_dict) {
                        serverLog(LL_WARNING, "RDB compatibility: Failed to create set dict for key=%s", key);
                        decrRefCount(o);
                        goto error_cleanup;
                    }
                    
                    /* Parse ziplist and add elements to set */
                    unsigned char *zl = (unsigned char *)o->ptr;
                    unsigned char *p = ziplistIndex(zl, 0);
                    while (p) {
                        unsigned char *vstr;
                        unsigned int vlen;
                        long long vlong;
                        
                        if (ziplistGet(p, &vstr, &vlen, &vlong)) {
                            sds element;
                            if (vstr) {
                                element = sdsnewlen(vstr, vlen);
                            } else {
                                element = sdsfromlonglong(vlong);
                            }
                            
                            if (dictAdd(set_dict, element, NULL) != DICT_OK) {
                                serverLog(LL_WARNING, "RDB compatibility: Duplicate set element for key=%s", key);
                                sdsfree(element);
                                dictRelease(set_dict);
                                decrRefCount(o);
                                goto error_cleanup;
                            }
                        }
                        p = ziplistNext(zl, p);
                    }
                    
                    /* Replace object pointer with the set dict */
                    zfree(o->ptr);
                    o->ptr = set_dict;
                    break;
                    
                default:
                    serverLog(LL_WARNING, "RDB compatibility: Unexpected RDB type %s for key=%s", getDataStructureType(rdbtype), key);
                    decrRefCount(o);
                    goto error_cleanup;
            }
            
            /* Update statistics for successful conversion */
            if (conversion_attempted) {
                rdbDowngradeStatsUpdateWithTypeAndVersion(1, converted_size, key, rdbtype, rdbver);
            }
            
            /* Log successful completion with timing */
            mstime_t elapsed = mstime() - start_time;
            serverLog(LL_VERBOSE, "RDB compatibility: Successfully processed key=%s, type=%s in %lldms", key, getDataStructureType(rdbtype), elapsed);
            
            /* Clear error and return successfully created object */
            if (error) *error = 0;
            return o;
        }
    }
    
    /* For other cases, fall back to original rdbLoadObject */
    serverLog(LL_DEBUG, "RDB compatibility: Using standard loader fallback for key=%s, type=%s rdb_v%d",
              key, getDataStructureType(rdbtype), rdbver);
    
    mstime_t fallback_elapsed = mstime() - start_time;
    serverLog(LL_DEBUG, "RDB compatibility: Fallback completed for key=%s in %lldms", key, fallback_elapsed);
    
    return rdbLoadObject(rdbtype, rdb, key);

error_cleanup:
    /* Comprehensive error cleanup with statistics tracking */
    if (encoded) {
        zfree(encoded);
        encoded = NULL;
    }
    
    /* Update failure statistics */
    if (conversion_attempted) {
        rdbDowngradeStatsUpdateWithTypeAndVersion(0, original_size, key, rdbtype, rdbver);
    }
    
    mstime_t error_elapsed = mstime() - start_time;
    serverLog(LL_WARNING, "RDB compatibility: Failed to process key=%s, type=%s after %lldms", key, getDataStructureType(rdbtype), error_elapsed);
    
    if (error) *error = RDB_LOAD_ERR_OTHER;
    return NULL;
}

/* Load stream with active_time support for RDB v11 compatibility */
robj *rdbLoadStreamWithActiveTime(rio *rdb, sds key, int *error, int rdbver) {
    robj *o = NULL;
    stream *s = NULL;
    
    serverLog(LL_VERBOSE, "RDB compatibility: Loading stream with active_time support for key=%s (RDB v%d)", key, rdbver);
    
    if (error) *error = RDB_LOAD_ERR_OTHER;
    
    /* Create stream object */
    o = createStreamObject();
    if (!o) {
        serverLog(LL_WARNING, "RDB compatibility: Failed to create stream object for key=%s", key);
        return NULL;
    }
    s = o->ptr;
    
    /* Load listpacks count */
    uint64_t listpacks = rdbLoadLen(rdb, NULL);
    if (listpacks == RDB_LENERR) {
        serverLog(LL_WARNING, "RDB compatibility: Failed to load listpacks count for stream key=%s", key);
        decrRefCount(o);
        return NULL;
    }
    
    /* Load all listpacks */
    while (listpacks--) {
        /* Load master ID */
        sds nodekey = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL);
        if (!nodekey) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to load stream master ID for key=%s", key);
            decrRefCount(o);
            return NULL;
        }
        
        if (sdslen(nodekey) != sizeof(streamID)) {
            serverLog(LL_WARNING, "RDB compatibility: Invalid stream node key size for key=%s", key);
            sdsfree(nodekey);
            decrRefCount(o);
            return NULL;
        }
        
        /* Load listpack */
        size_t lp_size;
        unsigned char *lp = rdbGenericLoadStringObject(rdb, RDB_LOAD_PLAIN, &lp_size);
        if (!lp) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to load stream listpack for key=%s", key);
            sdsfree(nodekey);
            decrRefCount(o);
            return NULL;
        }
        
        /* Validate listpack */
        
        /* Check if listpack is empty */
        unsigned char *first = lpFirst(lp);
        if (!first) {
            serverLog(LL_WARNING, "RDB compatibility: Empty listpack in stream for key=%s", key);
            sdsfree(nodekey);
            zfree(lp);
            decrRefCount(o);
            return NULL;
        }
        
        /* Insert into radix tree */
        if (!raxTryInsert(s->rax, (unsigned char*)nodekey, sizeof(streamID), lp, NULL)) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to insert listpack into stream radix tree for key=%s", key);
            sdsfree(nodekey);
            zfree(lp);
            decrRefCount(o);
            return NULL;
        }
        
        sdsfree(nodekey);
    }
    
    /* Load stream metadata */
    s->length = rdbLoadLen(rdb, NULL);
    s->last_id.ms = rdbLoadLen(rdb, NULL);
    s->last_id.seq = rdbLoadLen(rdb, NULL);
    
    if (rioGetReadError(rdb)) {
        serverLog(LL_WARNING, "RDB compatibility: Failed to load stream metadata for key=%s", key);
        decrRefCount(o);
        return NULL;
    }
    
    /* Load consumer groups */
    uint64_t cgroups_count = rdbLoadLen(rdb, NULL);
    if (cgroups_count == RDB_LENERR) {
        serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer groups count for stream key=%s", key);
        decrRefCount(o);
        return NULL;
    }
    
    while (cgroups_count--) {
        /* Load consumer group name and ID */
        streamID cg_id;
        sds cgname = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL);
        if (!cgname) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer group name for stream key=%s", key);
            decrRefCount(o);
            return NULL;
        }
        
        cg_id.ms = rdbLoadLen(rdb, NULL);
        cg_id.seq = rdbLoadLen(rdb, NULL);
        if (rioGetReadError(rdb)) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer group ID for stream key=%s", key);
            sdsfree(cgname);
            decrRefCount(o);
            return NULL;
        }
        
        /* Create consumer group */
        streamCG *cgroup = streamCreateCG(s, cgname, sdslen(cgname), &cg_id);
        if (!cgroup) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to create consumer group for stream key=%s", key);
            sdsfree(cgname);
            decrRefCount(o);
            return NULL;
        }
        sdsfree(cgname);
        
        /* Load global PEL */
        uint64_t pel_size = rdbLoadLen(rdb, NULL);
        if (pel_size == RDB_LENERR) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to load PEL size for stream key=%s", key);
            decrRefCount(o);
            return NULL;
        }
        
        while (pel_size--) {
            unsigned char rawid[sizeof(streamID)];
            if (rioRead(rdb, rawid, sizeof(rawid)) == 0) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to load PEL ID for stream key=%s", key);
                decrRefCount(o);
                return NULL;
            }
            
            streamNACK *nack = streamCreateNACK(NULL);
            nack->delivery_time = rdbLoadMillisecondTime(rdb, RDB_VERSION);
            nack->delivery_count = rdbLoadLen(rdb, NULL);
            if (rioGetReadError(rdb)) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to load NACK data for stream key=%s", key);
                decrRefCount(o);
                streamFreeNACK(nack);
                return NULL;
            }
            
            if (!raxTryInsert(cgroup->pel, rawid, sizeof(rawid), nack, NULL)) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to insert PEL entry for stream key=%s", key);
                decrRefCount(o);
                streamFreeNACK(nack);
                return NULL;
            }
        }
        
        /* Load consumers with active_time compatibility */
        uint64_t consumers_num = rdbLoadLen(rdb, NULL);
        if (consumers_num == RDB_LENERR) {
            serverLog(LL_WARNING, "RDB compatibility: Failed to load consumers count for stream key=%s", key);
            decrRefCount(o);
            return NULL;
        }
        
        while (consumers_num--) {
            sds cname = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL);
            if (!cname) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer name for stream key=%s", key);
                decrRefCount(o);
                return NULL;
            }
            
            streamConsumer *consumer = streamLookupConsumer(cgroup, cname, SLC_NONE);
            sdsfree(cname);
            
            /* Load seen_time */
            consumer->seen_time = rdbLoadMillisecondTime(rdb, RDB_VERSION);
            if (rioGetReadError(rdb)) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer seen_time for stream key=%s", key);
                decrRefCount(o);
                return NULL;
            }
            
            /* CRITICAL: Handle RDB v11 active_time field */
            if (rdbver >= RDB_VERSION_VALKEY_80) {
                /* RDB v11 has active_time field - read and discard it, set active_time = seen_time */
                long long active_time = rdbLoadMillisecondTime(rdb, RDB_VERSION);
                if (rioGetReadError(rdb)) {
                    serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer active_time for stream key=%s", key);
                    decrRefCount(o);
                    return NULL;
                }
                
                /* For Redis 6.0 compatibility, we don't have active_time field, so we ignore it */
                /* The consumer struct in Redis 6.0 only has seen_time */
                serverLog(LL_DEBUG, "RDB compatibility: Read and discarded active_time=%lld for consumer in stream key=%s", active_time, key);
            }
            
            /* Load consumer PEL */
            uint64_t consumer_pel_size = rdbLoadLen(rdb, NULL);
            if (consumer_pel_size == RDB_LENERR) {
                serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer PEL size for stream key=%s", key);
                decrRefCount(o);
                return NULL;
            }
            
            while (consumer_pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                if (rioRead(rdb, rawid, sizeof(rawid)) == 0) {
                    serverLog(LL_WARNING, "RDB compatibility: Failed to load consumer PEL ID for stream key=%s", key);
                    decrRefCount(o);
                    return NULL;
                }
                
                streamNACK *nack = raxFind(cgroup->pel, rawid, sizeof(rawid));
                if (nack == raxNotFound) {
                    serverLog(LL_WARNING, "RDB compatibility: Consumer PEL entry not found in global PEL for stream key=%s", key);
                    decrRefCount(o);
                    return NULL;
                }
                
                nack->consumer = consumer;
                if (!raxTryInsert(consumer->pel, rawid, sizeof(rawid), nack, NULL)) {
                    serverLog(LL_WARNING, "RDB compatibility: Failed to insert consumer PEL entry for stream key=%s", key);
                    decrRefCount(o);
                    return NULL;
                }
            }
        }
    }
    
    serverLog(LL_NOTICE, "RDB compatibility: Successfully loaded stream with active_time compatibility for key=%s", key);
    
    if (error) *error = 0;
    return o;
}

/* callback for listpackValidateIntegrity.
 * The listpack element pointed by 'p' will be converted and stored into ziplist. */
static int _listpackEntryConvertAndValidate(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(head_count);
    unsigned char *str;
    int64_t vlen;
    unsigned char intbuf[LP_INTBUF_SIZE];
    unsigned char **zl = (unsigned char**)userdata;

    str = lpGet(p, &vlen, intbuf);
    *zl = ziplistPush(*zl, str, vlen, ZIPLIST_TAIL);
    return 1;
}

/* Validate the integrity of the data structure while converting it to
 * ziplist and storing it at 'zl'.
 * The function is safe to call on non-validated listpacks, it returns 0
 * when encounter an integrity validation issue. */
int quicklistConvertAndValidateIntegrity(unsigned char *lp, unsigned char **zl) {
    unsigned char *p = lpFirst(lp);
    while(p) {
        _listpackEntryConvertAndValidate(p, 0, zl);
        p = lpNext(lp,p);
    }
    return 1;
}
