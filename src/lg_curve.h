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

    /*
     * Fixed duty used in manual mode, 0..100. Only read when the application is
     * in manual rather than automatic mode, where the fan holds this speed
     * regardless of temperature instead of following the points.
     *
     * It lives beside the curve rather than beside the control because the
     * curve is already the per-control policy object: it carries the same
     * enable flag, the same floor and the same source, and putting the manual
     * duty anywhere else would mean a second parallel structure that has to be
     * migrated, saved and reset alongside this one.
     *
     * -1 means no manual speed has ever been set for this control. That is not
     * the same as 0, and it is deliberately distinct from the initial value
     * too: a control the user never touched should be left alone in manual mode
     * rather than having a speed invented for it, because inventing one means
     * either a fan stopped or a fan at full speed, on hardware nobody has
     * looked at. The control falls back to whatever the curve would have done.
     */
    int manual_duty;

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

/*
 * Decide the duty one control should be written at this cycle.
 *
 * Shared by the interface and the headless daemon on purpose. The two are
 * separate programs that both own the fans at different times, and if they
 * each decided what a mode meant they would eventually disagree -- so the
 * machine's cooling would depend on whether a window happened to be open.
 *
 * Three inputs decide it, in this order:
 *   1. Failsafe, above the configured threshold, is always 100%. It is not
 *      gated on the mode, on auto_apply, or on the curve being enabled,
 *      because the one thing that must not be disabled is the thing that
 *      stops the machine cooking.
 *   2. In automatic mode the curve is evaluated against the source
 *      temperature, and a NaN source means no write rather than a guess.
 *   3. In manual mode the control holds its own fixed duty, so it does not
 *      move with temperature at all.
 *
 * min_duty applies in both modes. It is a floor the user set to keep a fan
 * turning, and honouring it in manual mode means a stray zero cannot stop a
 * header; ignoring it would let the one mode with no curve in it become the
 * one mode that can stall a fan.
 *
 * Returns false when the caller should not write this control at all.
 */
bool lg_curve_resolve_duty(const lg_curve *curve, bool manual_mode, bool failsafe,
                           double source_temp, int *out_duty);
