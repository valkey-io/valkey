#ifndef __GEO_H__
#define __GEO_H__

#include "server.h"

#define MAX_GEO_ARRAY_BUFFER 8

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
typedef struct geoPoint {
    double longitude;
    double latitude;
    double dist;
    double score;
    char *member;
} geoPoint;

typedef struct geoArray {
    struct geoPoint *array;
    size_t buckets;
    size_t used;
    struct geoPoint arraybuf[MAX_GEO_ARRAY_BUFFER]; /* Pre-allocated buffer reduces heap allocation */
} geoArray;

#endif
