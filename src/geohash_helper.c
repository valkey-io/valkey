/*
 * Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>.
 * Copyright (c) 2015-2016, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/* This is a C++ to C conversion from the ardb project.
 * This file started out as:
 * https://github.com/yinqiwen/ardb/blob/d42503/src/geo/geohash_helper.cpp
 */

#include "fmacros.h"
#include "geohash_helper.h"
#include "debugmacro.h"
#include <math.h>

#define D_R (M_PI / 180.0)
#define R_MAJOR 6378137.0
#define R_MINOR 6356752.3142
#define RATIO (R_MINOR / R_MAJOR)
#define ECCENT (sqrt(1.0 - (RATIO * RATIO)))
#define COM (0.5 * ECCENT)

/// @brief Earth's quadratic mean radius for WGS-84
const double EARTH_RADIUS_IN_METERS = 6372797.560856;

const double MERCATOR_MAX = 20037726.37;
const double MERCATOR_MIN = -20037726.37;

static inline double deg_rad(double ang) {
    return ang * D_R;
}
static inline double rad_deg(double ang) {
    return ang / D_R;
}

/* This function is used in order to estimate the step (bits precision)
 * of the 9 search area boxes during radius queries. */
uint8_t geohashEstimateStepsByRadius(double range_meters, double lat) {
    if (range_meters == 0) return 26;
    int step = 1;
    while (range_meters < MERCATOR_MAX) {
        range_meters *= 2;
        step++;
    }
    step -= 2; /* Make sure range is included in most of the base cases. */

    /* Wider range towards the poles... Note: it is possible to do better
     * than this approximation by computing the distance between meridians
     * at this latitude, but this does the trick for now. */
    if (lat > 66 || lat < -66) {
        step--;
        if (lat > 80 || lat < -80) step--;
    }

    /* Frame to valid range. */
    if (step < 1) step = 1;
    if (step > 26) step = 26;
    return step;
}

/* Return the bounding box of the search area by shape (see geohash.h GeoShape)
 * bounds[0] - bounds[2] is the minimum and maximum longitude
 * while bounds[1] - bounds[3] is the minimum and maximum latitude.
 * since the higher the latitude, the shorter the arc length, the box shape is as follows
 * (left and right edges are actually bent), as shown in the following diagram:
 *
 *    \-----------------/          --------               \-----------------/
 *     \               /         /          \              \               /
 *      \  (long,lat) /         / (long,lat) \              \  (long,lat) /
 *       \           /         /              \             /             \
 *         ---------          /----------------\           /---------------\
 *  Northern Hemisphere       Southern Hemisphere         Around the equator
 *
 * Note: In case of the BYPOLYGON search, this function also sets the centroid coordinates in the shape.
 */
int geohashBoundingBox(GeoShape *shape, double *bounds) {
    if (!bounds) return 0;
    double height = 0.0, width = 0.0;
    if (shape->type == CIRCULAR_TYPE) {
        height = shape->conversion * shape->t.radius;
        width = shape->conversion * shape->t.radius;
    } else if (shape->type == RECTANGLE_TYPE) {
        height = shape->conversion * shape->t.r.height / 2;
        width = shape->conversion * shape->t.r.width / 2;
    } else if (shape->type == POLYGON_TYPE) {
        int num_vertices = shape->t.polygon.num_vertices;
        double x = 0.0, y = 0.0, z = 0.0;
        /* Bounding box directly from lon & lat. */
        double min_lon = GEO_LONG_MAX, max_lon = GEO_LONG_MIN;
        double min_lat = GEO_LAT_MAX, max_lat = GEO_LAT_MIN;
        for (int i = 0; i < num_vertices; i++) {
            double longitude = shape->t.polygon.points[i][0];
            double latitude = shape->t.polygon.points[i][1];
            /* Calculate the bounding box (in LON/LAT). */
            if (longitude < min_lon) min_lon = longitude;
            if (longitude > max_lon) max_lon = longitude;
            if (latitude < min_lat) min_lat = latitude;
            if (latitude > max_lat) max_lat = latitude;
            /* Convert to cartesian coordinates and accumulate for centroid.
             * Note: We do not need to divide the x, y & z values by num_vertices because the magnitude is not needed
             * for centroid calculation. Summing the cartesian coordinates is all that is needed for computing the angle
             * which can be converted back into the LON/LAT format. */
            double lon_rad = deg_rad(longitude);
            double lat_rad = deg_rad(latitude);
            double cur_x = cos(lat_rad) * cos(lon_rad);
            double cur_y = cos(lat_rad) * sin(lon_rad);
            double cur_z = sin(lat_rad);
            x += cur_x;
            y += cur_y;
            z += cur_z;
        }
        /* Set bounding box. */
        bounds[0] = min_lon;
        bounds[1] = min_lat;
        bounds[2] = max_lon;
        bounds[3] = max_lat;
        /* Compute centroid radians from the summed cartesian coords. The centroid is used as the starting coord. */
        double central_lon = atan2(y, x);
        double central_hyp = sqrt(x * x + y * y);
        double central_lat = atan2(z, central_hyp);
        shape->xy[0] = rad_deg(central_lon);
        shape->xy[1] = rad_deg(central_lat);
        return 1;
    }
    double longitude = shape->xy[0];
    double latitude = shape->xy[1];
    const double lat_delta = rad_deg(height / EARTH_RADIUS_IN_METERS);
    const double long_delta_top = rad_deg(width / EARTH_RADIUS_IN_METERS / cos(deg_rad(latitude + lat_delta)));
    const double long_delta_bottom = rad_deg(width / EARTH_RADIUS_IN_METERS / cos(deg_rad(latitude - lat_delta)));
    /* The directions of the northern and southern hemispheres
     * are opposite, so we choice different points as min/max long/lat */
    int southern_hemisphere = latitude < 0 ? 1 : 0;
    bounds[0] = southern_hemisphere ? longitude - long_delta_bottom : longitude - long_delta_top;
    bounds[2] = southern_hemisphere ? longitude + long_delta_bottom : longitude + long_delta_top;
    bounds[1] = latitude - lat_delta;
    bounds[3] = latitude + lat_delta;
    return 1;
}

/* Calculate a set of areas (center + 8) that are able to cover a range query
 * for the specified position and shape (see geohash.h GeoShape).
 * the bounding box saved in shaple.bounds */
GeoHashRadius geohashCalculateAreasByShapeWGS84(GeoShape *shape) {
    GeoHashRange long_range, lat_range;
    GeoHashRadius radius;
    GeoHashBits hash;
    GeoHashNeighbors neighbors;
    GeoHashArea area;
    double min_lon, max_lon, min_lat, max_lat;
    int steps;

    geohashBoundingBox(shape, shape->bounds);
    min_lon = shape->bounds[0];
    min_lat = shape->bounds[1];
    max_lon = shape->bounds[2];
    max_lat = shape->bounds[3];

    double longitude = shape->xy[0];
    double latitude = shape->xy[1];
    /* radius_meters is calculated differently in different search types:
     * 1) CIRCULAR_TYPE, just use radius.
     * 2) RECTANGLE_TYPE, we use sqrt((width/2)^2 + (height/2)^2) to
     * calculate the distance from the center point to the corner
     * 3) POLYGON_TYPE, use height/width from the bounding box and
     * use the centroid (as center) to calculate the distance. */
    double radius_meters = 0.0;
    if (shape->type == CIRCULAR_TYPE) {
        /* For circular shapes, use the given radius directly. */
        radius_meters = shape->t.radius;
    } else if (shape->type == RECTANGLE_TYPE) {
        /* For rectangles, calculate the diagonal as the radius. */
        radius_meters = sqrt((shape->t.r.width / 2) * (shape->t.r.width / 2) + (shape->t.r.height / 2) * (shape->t.r.height / 2));
    } else if (shape->type == POLYGON_TYPE) {
        /* For polygons, use max distance from the centroid to the bounding box. */
        double dist_top_left = geohashGetDistance(longitude, latitude, min_lon, max_lat);
        double dist_top_right = geohashGetDistance(longitude, latitude, max_lon, max_lat);
        double dist_bottom_left = geohashGetDistance(longitude, latitude, min_lon, min_lat);
        double dist_bottom_right = geohashGetDistance(longitude, latitude, max_lon, min_lat);
        /* Find the maximum distance (which will be the radius that covers the whole bounding box). */
        radius_meters = dist_top_left;
        if (dist_top_right > radius_meters) radius_meters = dist_top_right;
        if (dist_bottom_left > radius_meters) radius_meters = dist_bottom_left;
        if (dist_bottom_right > radius_meters) radius_meters = dist_bottom_right;
    }
    radius_meters *= shape->conversion;

    steps = geohashEstimateStepsByRadius(radius_meters, latitude);

    geohashGetCoordRange(&long_range, &lat_range);
    geohashEncode(&long_range, &lat_range, longitude, latitude, steps, &hash);
    geohashNeighbors(&hash, &neighbors);
    geohashDecode(long_range, lat_range, hash, &area);

    /* Check if the step is enough at the limits of the covered area.
     * Sometimes when the search area is near an edge of the
     * area, the estimated step is not small enough, since one of the
     * north / south / west / east square is too near to the search area
     * to cover everything. */
    int decrease_step = 0;
    {
        GeoHashArea north, south, east, west;

        geohashDecode(long_range, lat_range, neighbors.north, &north);
        geohashDecode(long_range, lat_range, neighbors.south, &south);
        geohashDecode(long_range, lat_range, neighbors.east, &east);
        geohashDecode(long_range, lat_range, neighbors.west, &west);

        if (north.latitude.max < max_lat) decrease_step = 1;
        if (south.latitude.min > min_lat) decrease_step = 1;
        if (east.longitude.max < max_lon) decrease_step = 1;
        if (west.longitude.min > min_lon) decrease_step = 1;
    }

    if (steps > 1 && decrease_step) {
        steps--;
        geohashEncode(&long_range, &lat_range, longitude, latitude, steps, &hash);
        geohashNeighbors(&hash, &neighbors);
        geohashDecode(long_range, lat_range, hash, &area);
    }

    /* Exclude the search areas that are useless. */
    if (steps >= 2) {
        if (area.latitude.min < min_lat) {
            GZERO(neighbors.south);
            GZERO(neighbors.south_west);
            GZERO(neighbors.south_east);
        }
        if (area.latitude.max > max_lat) {
            GZERO(neighbors.north);
            GZERO(neighbors.north_east);
            GZERO(neighbors.north_west);
        }
        if (area.longitude.min < min_lon) {
            GZERO(neighbors.west);
            GZERO(neighbors.south_west);
            GZERO(neighbors.north_west);
        }
        if (area.longitude.max > max_lon) {
            GZERO(neighbors.east);
            GZERO(neighbors.south_east);
            GZERO(neighbors.north_east);
        }
    }
    radius.hash = hash;
    radius.neighbors = neighbors;
    radius.area = area;
    return radius;
}

GeoHashFix52Bits geohashAlign52Bits(const GeoHashBits hash) {
    uint64_t bits = hash.bits;
    bits <<= (52 - hash.step * 2);
    return bits;
}

/* Calculate distance using simplified haversine great circle distance formula.
 * Given longitude diff is 0 the asin(sqrt(a)) on the haversine is asin(sin(abs(u))).
 * arcsin(sin(x)) equal to x when x ∈[−𝜋/2,𝜋/2]. Given latitude is between [−𝜋/2,𝜋/2]
 * we can simplify arcsin(sin(x)) to x.
 */
double geohashGetLatDistance(double lat1d, double lat2d) {
    return EARTH_RADIUS_IN_METERS * fabs(deg_rad(lat2d) - deg_rad(lat1d));
}

/* Calculate distance using haversine great circle distance formula. */
double geohashGetDistance(double lon1d, double lat1d, double lon2d, double lat2d) {
    double lat1r, lon1r, lat2r, lon2r, u, v, a;
    lon1r = deg_rad(lon1d);
    lon2r = deg_rad(lon2d);
    v = sin((lon2r - lon1r) / 2);
    /* Reflects about 6nm on earth for comparing longitudes. */
    const double GEO_EPSILON = 1e-15;
    /* if v == 0, or practically 0, we can avoid doing expensive math when longitudes are practically the same */
    if (fabs(v) <= GEO_EPSILON) return geohashGetLatDistance(lat1d, lat2d);
    lat1r = deg_rad(lat1d);
    lat2r = deg_rad(lat2d);
    u = sin((lat2r - lat1r) / 2);
    a = u * u + cos(lat1r) * cos(lat2r) * v * v;
    return 2.0 * EARTH_RADIUS_IN_METERS * asin(sqrt(a));
}

int geohashGetDistanceIfInRadius(double x1, double y1, double x2, double y2, double radius, double *distance) {
    *distance = geohashGetDistance(x1, y1, x2, y2);
    if (*distance > radius) return 0;
    return 1;
}

int geohashGetDistanceIfInRadiusWGS84(double x1, double y1, double x2, double y2, double radius, double *distance) {
    return geohashGetDistanceIfInRadius(x1, y1, x2, y2, radius, distance);
}

/* Judge whether a point is in the axis-aligned rectangle, when the distance
 * between a searched point and the center point is less than or equal to
 * height/2 or width/2 in height and width, the point is in the rectangle.
 *
 * width_m, height_m: the rectangle
 * x1, y1 : the center of the box
 * x2, y2 : the point to be searched
 */
int geohashGetDistanceIfInRectangle(double width_m,
                                    double height_m,
                                    double x1,
                                    double y1,
                                    double x2,
                                    double y2,
                                    double *distance) {
    /* latitude distance is less expensive to compute than longitude distance
     * so we check first for the latitude condition */
    double lat_distance = geohashGetLatDistance(y2, y1);
    if (lat_distance > height_m / 2) {
        return 0;
    }
    double lon_distance = geohashGetDistance(x2, y2, x1, y2);
    if (lon_distance > width_m / 2) {
        return 0;
    }
    *distance = geohashGetDistance(x1, y1, x2, y2);
    return 1;
}

/* Check if `point` is inside a polygon (defined by `vertices` where each vertex's index 0 is lon & 1 is lat) using
 * ray casting and calculate the distance from the centroid to the point.
 * The Polygon's centroid's lon lat coordinates are `centroidLon` and `centroidLat`.
 * The algorithm is based on PNPOLY - Point Inclusion in Polygon Test by W. Randolph Franklin (WRF).
 * See: https://wrfranklin.org/Research/Short_Notes/pnpoly.html
 * Returns 1 if inside the polyon and returns 0 otherwise. */
int geohashGetDistanceIfInPolygon(double centroidLon, double centroidLat, double *point, double (*vertices)[2], int num_vertices, double *distance) {
    int i, j;
    int inside = 0;
    for (i = 0, j = num_vertices - 1; i < num_vertices; j = i++) {
        double *vertexA = vertices[i];
        double *vertexB = vertices[j];
        if (((vertexA[1] > point[1]) != (vertexB[1] > point[1])) &&
            (point[0] < (vertexB[0] - vertexA[0]) * (point[1] - vertexA[1]) / (vertexB[1] - vertexA[1]) + vertexA[0])) {
            inside = !inside;
        }
    }
    if (inside) {
        *distance = geohashGetDistance(centroidLon, centroidLat, point[0], point[1]);
    }
    return inside;
}
