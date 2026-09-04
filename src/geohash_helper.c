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

/* Compute the bounding box and centroid for a set of points.
 * If max_dist_m > 0, the bounding box is expanded by that distance in all directions (used for bypath).
 * The centroid is stored in shape->xy[] and the bounding box in bounds[]. */
static int computeBoundingBoxAndCentroid(double (*points)[2], int count, double max_dist_m, GeoShape *shape, double *bounds) {
    double x = 0.0, y = 0.0, z = 0.0;
    double min_lon = GEO_LONG_MAX, max_lon = GEO_LONG_MIN;
    double min_lat = GEO_LAT_MAX, max_lat = GEO_LAT_MIN;
    for (int i = 0; i < count; i++) {
        double longitude = points[i][0];
        double latitude = points[i][1];
        if (longitude < min_lon) min_lon = longitude;
        if (longitude > max_lon) max_lon = longitude;
        if (latitude < min_lat) min_lat = latitude;
        if (latitude > max_lat) max_lat = latitude;
        /* Accumulate cartesian coordinates for centroid calculation.
         * We do not need to divide by count because only the direction
         * (angle) matters, not the magnitude. */
        double lon_rad = deg_rad(longitude);
        double lat_rad = deg_rad(latitude);
        x += cos(lat_rad) * cos(lon_rad);
        y += cos(lat_rad) * sin(lon_rad);
        z += sin(lat_rad);
    }
    if (max_dist_m > 0) {
        /* This applies to bypath and expands bounding box by the buffer distance. Use the latitude with
         * the smallest cosine (highest absolute value) to ensure the longitude
         * expansion is never underestimated. */
        double lat_delta = rad_deg(max_dist_m / EARTH_RADIUS_IN_METERS);
        double extreme_lat = fabs(min_lat) > fabs(max_lat) ? min_lat : max_lat;
        double adjusted_extreme_lat = extreme_lat + (extreme_lat > 0 ? lat_delta : -lat_delta);
        if (adjusted_extreme_lat > GEO_LAT_MAX) adjusted_extreme_lat = GEO_LAT_MAX;
        if (adjusted_extreme_lat < -GEO_LAT_MAX) adjusted_extreme_lat = -GEO_LAT_MAX;
        double lon_delta = rad_deg(max_dist_m / EARTH_RADIUS_IN_METERS / cos(deg_rad(adjusted_extreme_lat)));
        min_lon -= lon_delta;
        min_lat -= lat_delta;
        max_lon += lon_delta;
        max_lat += lat_delta;
    }
    bounds[0] = min_lon;
    bounds[1] = min_lat;
    bounds[2] = max_lon;
    bounds[3] = max_lat;
    /* Compute centroid from cartesian coords. */
    double central_lon = atan2(y, x);
    double central_hyp = sqrt(x * x + y * y);
    double central_lat = atan2(z, central_hyp);
    shape->xy[0] = rad_deg(central_lon);
    shape->xy[1] = rad_deg(central_lat);
    /* When a shape crosses the antimeridian, the cartesian centroid longitude
     * can land at exactly ±180° which overflows geohashEncode (it treats 180°
     * as the exclusive upper bound). Nudge it slightly into valid range.
     * This is equivalent to ~1cm on the Earth's surface. */
    if (shape->xy[0] >= GEO_LONG_MAX) shape->xy[0] = GEO_LONG_MAX - 1e-10;
    if (shape->xy[0] < GEO_LONG_MIN) shape->xy[0] = GEO_LONG_MIN;
    return 1;
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
 * Note: In case of the BYPOLYGON or BYPATH search, this function also sets the centroid coordinates in the shape.
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
        return computeBoundingBoxAndCentroid(shape->t.polygon.points, shape->t.polygon.num_vertices, 0, shape, bounds);
    } else if (shape->type == PATH_TYPE) {
        double max_dist_m = shape->conversion * shape->t.path.width;
        return computeBoundingBoxAndCentroid(shape->t.path.points, shape->t.path.num_points, max_dist_m, shape, bounds);
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
    } else if (shape->type == POLYGON_TYPE || shape->type == PATH_TYPE) {
        /* For polygons and paths, use max distance from the centroid to the bounding box corners.
         * For paths, the bounding box already includes the buffer expansion. */
        double dist_top_left = geohashGetDistance(longitude, latitude, min_lon, max_lat);
        double dist_top_right = geohashGetDistance(longitude, latitude, max_lon, max_lat);
        double dist_bottom_left = geohashGetDistance(longitude, latitude, min_lon, min_lat);
        double dist_bottom_right = geohashGetDistance(longitude, latitude, max_lon, min_lat);
        radius_meters = dist_top_left;
        if (dist_top_right > radius_meters) radius_meters = dist_top_right;
        if (dist_bottom_left > radius_meters) radius_meters = dist_bottom_left;
        if (dist_bottom_right > radius_meters) radius_meters = dist_bottom_right;
    }
    /* For CIRCULAR and RECTANGLE types, radius_meters is in the shape's unit
     * and needs conversion to meters. For POLYGON and PATH types, the distance
     * is already computed in meters via geohashGetDistance(). */
    if (shape->type == CIRCULAR_TYPE || shape->type == RECTANGLE_TYPE) {
        radius_meters *= shape->conversion;
    }

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
 * Returns 1 if inside the polygon and returns 0 otherwise. */
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

/* Pre-compute per-segment bounding boxes and Haversine lengths.
 * Called once per BYPATH command before iterating over candidates.
 *
 * Unlike computeBoundingBoxAndCentroid (which produces one bbox for the whole
 * path to select geohash cells), this produces one bbox per segment to skip
 * distant segments during the per-candidate distance check.
 *
 * For each segment, we compute an axis-aligned bounding box expanded by the
 * buffer distance in all directions. During the search, a candidate point
 * that falls outside a segment's bbox cannot be within max_dist_m of that
 * segment, so the expensive projection + Haversine can be skipped (4 double
 * comparisons vs ~8 trig ops).
 *
 * The latitude expansion (lat_delta) is constant everywhere. The longitude
 * expansion (lon_delta) is computed at the segment's most extreme latitude
 * (pushed outward by lat_delta) to ensure the bbox is never too narrow.
 *
 * seg_lengths[] stores pre-computed Haversine segment lengths to avoid
 * redundant computation in WITHPATHDIST mode (which accumulates along-path
 * distance). */
void geohashPrecomputePathSegments(double (*points)[2], int num_points, double max_dist_m, double (*seg_bboxes)[4], double *seg_lengths) {
    double lat_delta = rad_deg(max_dist_m / EARTH_RADIUS_IN_METERS);
    for (int s = 0; s < num_points - 1; s++) {
        double lon0 = points[s][0], lat0 = points[s][1];
        double lon1 = points[s + 1][0], lat1 = points[s + 1][1];

        /* Tight bbox around the segment endpoints. */
        double smin_lon = lon0 < lon1 ? lon0 : lon1;
        double smax_lon = lon0 > lon1 ? lon0 : lon1;
        double smin_lat = lat0 < lat1 ? lat0 : lat1;
        double smax_lat = lat0 > lat1 ? lat0 : lat1;

        /* Compute lon_delta at the latitude farthest from the equator
         * (after expansion) to ensure the bbox is conservative. */
        double extreme_lat = fabs(smin_lat) > fabs(smax_lat) ? smin_lat : smax_lat;
        double adj_lat = extreme_lat + (extreme_lat > 0 ? lat_delta : -lat_delta);
        if (adj_lat > GEO_LAT_MAX) adj_lat = GEO_LAT_MAX;
        if (adj_lat < -GEO_LAT_MAX) adj_lat = -GEO_LAT_MAX;
        double lon_delta = rad_deg(max_dist_m / EARTH_RADIUS_IN_METERS / cos(deg_rad(adj_lat)));

        /* Expand bbox by buffer distance. */
        seg_bboxes[s][0] = smin_lon - lon_delta;
        seg_bboxes[s][1] = smin_lat - lat_delta;
        seg_bboxes[s][2] = smax_lon + lon_delta;
        seg_bboxes[s][3] = smax_lat + lat_delta;

        /* Pre-compute segment length (Haversine) for along-path accumulation. */
        seg_lengths[s] = geohashGetDistance(lon0, lat0, lon1, lat1);
    }
}

/* Check if a point is within a given distance of a polyline (path).
 * The algorithm computes the minimum perpendicular distance from the point
 * to any segment of the path. If that distance is <= max_dist_m, the point
 * is considered within the path corridor.
 *
 * For each segment A->B, we project the point P onto the line and clamp
 * the parameter t to [0,1] to find the closest point on the segment.
 * We then use Haversine to compute the great-circle distance.
 *
 * Optimization: per-segment bounding box pruning. Each segment has a
 * pre-computed bbox (geohashPrecomputePathSegments) expanded by the buffer distance.
 *
 *   bbox[0]       bbox[1]       bbox[2]       bbox[3]       bbox[4]
 *   +-------+    +-------+    +-------+    +-------+    +-------+
 *   | seg 0 |    | seg 1 |    | seg 2 |    | seg 3 |    | seg 4 |
 *   |  A->B |    |  B->C |    |  C->D |    |  D->E |    |  E->F |
 *   |       |    |   P   |    |       |    |       |    |       |
 *   +-------+    +-------+    +-------+    +-------+    +-------+
 *
 *   If P is inside bbox[1] only, then full calculation for seg 1 and skip rest.
 *
 * Returns 1 if the point is within the path corridor, 0 otherwise.
 * When dist_type == GEO_DIST_NONE, sets no distance and exit early.
 * When dist_type == GEO_DIST_PATHDIST, sets *distance to the along-path
 * distance from the first vertex to the nearest projection point.
 * When dist_type == GEO_DIST_NEAREST, sets *distance to the perpendicular
 * distance from the point to the nearest segment. */
int geohashGetDistanceIfInPath(double *point,
                               double (*pathPoints)[2],
                               int num_points,
                               double max_dist_m,
                               int dist_type,
                               double (*seg_bboxes)[4],
                               double *seg_lengths,
                               double *distance) {
    double min_dist = INFINITY;
    double along_path_at_min = 0.0; /* Along-path distance from first vertex to the
                                     * projection point of the closest segment. */
    double cumulative_len = 0.0;

    for (int i = 0; i < num_points - 1; i++) {
        double seg_len = seg_lengths ? seg_lengths[i] : 0.0;

        /* Fast bbox rejection: skip segments whose expanded bbox doesn't contain the point. */
        if (seg_bboxes) {
            double *bb = seg_bboxes[i];
            if (point[0] < bb[0] || point[0] > bb[2] ||
                point[1] < bb[1] || point[1] > bb[3]) {
                cumulative_len += seg_len;
                continue;
            }
        }

        double ax = pathPoints[i][0], ay = pathPoints[i][1];
        double bx = pathPoints[i + 1][0], by = pathPoints[i + 1][1];

        /* Project point onto the segment using equirectangular approximation.
         * Scale longitude differences by cos(latitude) to account for the
         * convergence of meridians at higher latitudes. This gives a locally
         * correct metric for finding the nearest point on the segment. */
        double cos_lat = cos(deg_rad((ay + by) / 2.0));

        /* Normalize longitude differences to handle antimeridian crossing.
         * Without this, a segment from 179° to -179° would compute dx as
         * -358° instead of the correct +2°. */
        double raw_dx = bx - ax;
        if (raw_dx > 180.0) raw_dx -= 360.0;
        if (raw_dx < -180.0) raw_dx += 360.0;
        double dx = raw_dx * cos_lat;
        double dy = by - ay;
        double len_sq = dx * dx + dy * dy;
        double t;

        if (len_sq < 1e-20) {
            /* Degenerate segment (A == B), use endpoint. */
            t = 0.0;
        } else {
            double raw_px = point[0] - ax;
            if (raw_px > 180.0) raw_px -= 360.0;
            if (raw_px < -180.0) raw_px += 360.0;
            double px = raw_px * cos_lat;
            double py = (point[1] - ay);
            t = (px * dx + py * dy) / len_sq;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
        }

        /* Closest point on segment (in lon/lat for Haversine).
         * Use the normalized longitude difference for interpolation to
         * correctly handle antimeridian crossing. */
        double cx = ax + t * raw_dx;
        /* Wrap cx back into [-180, 180] range. */
        if (cx >= 180.0) cx -= 360.0;
        if (cx < -180.0) cx += 360.0;
        double cy = ay + t * (by - ay);

        /* Haversine distance from point to closest point on segment. */
        double dist = geohashGetDistance(point[0], point[1], cx, cy);
        if (dist < min_dist) {
            min_dist = dist;
            if (dist_type == GEO_DIST_NONE && min_dist <= max_dist_m) {
                return 1;
            }
            along_path_at_min = cumulative_len + t * seg_len;
        }
        cumulative_len += seg_len;
    }

    if (min_dist <= max_dist_m) {
        *distance = (dist_type == GEO_DIST_PATHDIST) ? along_path_at_min : min_dist;
        return 1;
    }
    return 0;
}
