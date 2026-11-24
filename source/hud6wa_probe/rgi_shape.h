/* rgi_shape — C interface to st1ng's Mapbox-style maneuver + lane pictures (third_party/.../src/hudshapes.h, C++, freestanding).
 * One call = one picture: shapeOf(type, angle, side, junction) [+ addLanes] -> mergePieces -> whole polygons mapped onto the box. */
#ifndef RGI_SHAPE_H
#define RGI_SHAPE_H
#ifdef __cplusplus
extern "C" {
#endif

#define RR_POLY_PTS 96               /* hudshapes kPolyPts; the cluster takes up to 99 */
typedef struct { unsigned char tone, n; short p[RR_POLY_PTS][2]; } RrPoly;    /* tone 0 = route, 1 = context (dim) */
typedef struct { unsigned status; int hl; int ang[4]; unsigned na; } RrLane; /* status 0 not good, 1 good, 2 preferred; hl 1000 none */

/* type = iAP2/CarPlay maneuver type 0..53, angle = exit angle (+ right), side 0 right-hand / 1 left-hand traffic, junction type
 * and road angles as the iPhone sends them, lanes left to right (n 0 = none). Box: where the 6WA box (70,20)-(170,120) goes.
 * Returns the polygons written to out (<= max), in drawing order. */
int rr_shape(unsigned type, int angle, unsigned side, unsigned jtype, const int *jang, unsigned njang,
             const RrLane *lanes, unsigned nlanes, int x0, int y0, int x1, int y1, RrPoly *out, int max);

#ifdef __cplusplus
}
#endif
#endif
