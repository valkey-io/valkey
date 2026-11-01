#ifndef __GEO_H__
#define __GEO_H__

#include "server.h"
#include "ordered_index.h"

#define MAX_GEO_ARRAY_BUFFER 8

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
typedef struct geoPoint {
    double longitude;
    double latitude;
    double dist;
    double score;
    /* An owned sds copy for listpack sorted sets, or a reference to the source
     * B+tree item, which stays valid until the reply or store is done. */
    union {
        sds str;
        OrderedIndexItem *item;
    } member;
} geoPoint;

typedef struct geoArray {
    struct geoPoint *array;
    size_t buckets;
    size_t used;
    int item_refs;                                  /* Members are B+tree item references, not owned sds. */
    struct geoPoint arraybuf[MAX_GEO_ARRAY_BUFFER]; /* Pre-allocated buffer reduces heap allocation */
} geoArray;

#endif
