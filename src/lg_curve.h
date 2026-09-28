/*
 * lg_curve - Bezier fan-curve evaluation.
 *
 * This is a faithful port of the curve maths in the legacy Python
 * implementation. Parity is enforced by tests/golden/curve_golden.json, which
 * was captured from the Python before this rewrite. Two details matter for
 * bit-exact agreement:
 *
 *   1. Python's round() is round-half-to-EVEN; C round() is half-away-from-zero.
 *      lg_py_round() implements the Python rule. Using round() here would put
 *      the two implementations a whole duty step apart on exact .5 boundaries.
 *   2. duty_from_curve() returns the FIRST sample bracket containing the
 *      temperature, in sample order, and breaks early. That is preserved
 *      exactly, including the non-monotonic-X case, rather than "improving" it
 *      into a binary search that would disagree with the shipped behaviour.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_CURVE_H
#define LG_CURVE_H

#include <stdbool.h>
#include <stddef.h>

#include "lg_model.h"

/* Editor bounds, matching the Python constants. */
#define LG_TEMP_MIN 20
#define LG_TEMP_MAX 90
#define LG_TEMP_HARD_MIN 0
#define LG_TEMP_HARD_MAX 110

/* Points beyond this are dropped on load; no sane curve needs more. */
#define LG_CURVE_MAX_POINTS 32
#define LG_CURVE_SAMPLES_PER_SEGMENT 32
#define LG_CURVE_MAX_SAMPLES ((LG_CURVE_MAX_POINTS - 1) * LG_CURVE_SAMPLES_PER_SEGMENT + 1)

typedef struct {
    int temp; /* degrees Celsius */
    int duty; /* percent */
} lg_point;

typedef struct {
    lg_point points[LG_CURVE_MAX_POINTS];
    size_t npoints;
    bool enabled;
    int min_duty; /* safety floor in percent; 0 means no floor */

    /* Sensor key this curve evaluates against. Empty selects the default
     * (CPU package) source. */
    char source[128];

    /* Cached sample table, rebuilt lazily whenever the points change. */
    double sx[LG_CURVE_MAX_SAMPLES];
    double sy[LG_CURVE_MAX_SAMPLES];
    size_t nsamples;
    bool samples_valid;
} lg_curve;

void lg_curve_init(lg_curve *curve);

/* Default profile for a control label: pumps get a higher floor. */
size_t lg_curve_default_points(lg_point *out, size_t cap, const char *label);

/*
 * Clamp and order points exactly as the Python did: temps are forced
 * non-decreasing and clamped to the hard range, duties clamped to 0..100.
 * Returns the number of points written (capped at LG_CURVE_MAX_POINTS).
 */
size_t lg_curve_normalize(const lg_point *points, size_t n, lg_point *out, size_t cap);

/* Replace the curve's points, re-normalising and invalidating the cache. */
void lg_curve_set_points(lg_curve *curve, const lg_point *points, size_t n);

/* Fill the cached sample table. Returns the sample count. */
size_t lg_curve_build_samples(lg_curve *curve);

/* Evaluate the curve, returning a duty percentage in 0..100. */
int lg_curve_duty(const lg_curve *curve, double temp_c);

/* Convenience: evaluate raw points without building a full curve object. */
int lg_curve_duty_points(const lg_point *points, size_t n, double temp_c);

#endif /* LG_CURVE_H */
