/*
 * lg_curve - Bezier fan-curve evaluation (port of the legacy Python maths).
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_curve.h"

#include <math.h>
#include <string.h>

/*
 * lg_py_round lives in lg_model so that duty readback shares the same
 * half-to-even rule as curve evaluation.
 */

void lg_curve_init(lg_curve *curve)
{
    if (curve == NULL) {
        return;
    }
    memset(curve, 0, sizeof(*curve));
    curve->enabled = true;
}

size_t lg_curve_default_points(lg_point *out, size_t cap, const char *label)
{
    static const lg_point fan_default[5] = {
        {30, 30}, {40, 38}, {50, 50}, {60, 72}, {72, 100},
    };
    static const lg_point pump_default[5] = {
        {30, 70}, {40, 78}, {50, 86}, {60, 94}, {72, 100},
    };

    const lg_point *src = fan_default;

    /* The Python matched a case-insensitive "pump" substring in the label. */
    if (label != NULL) {
        char lower[128];
        size_t n = 0;
        for (; label[n] != '\0' && n + 1 < sizeof(lower); n++) {
            char c = label[n];
            lower[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        lower[n] = '\0';

        const char *hit = strstr(lower, "pump");
        if (hit != NULL) {
            src = pump_default;
        }
    }

    size_t count = 5;
    if (out == NULL || cap < count) {
        return count;
    }
    memcpy(out, src, count * sizeof(*src));
    return count;
}

size_t lg_curve_normalize(const lg_point *points, size_t n, lg_point *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < n && written < cap; i++) {
        int temp = points[i].temp;
        int duty = points[i].duty;

        /* Force non-decreasing temperature against the previous point. */
        if (written > 0 && temp < out[written - 1].temp) {
            temp = out[written - 1].temp;
        }

        if (temp < LG_TEMP_HARD_MIN) {
            temp = LG_TEMP_HARD_MIN;
        } else if (temp > LG_TEMP_HARD_MAX) {
            temp = LG_TEMP_HARD_MAX;
        }

        if (duty < 0) {
            duty = 0;
        } else if (duty > 100) {
            duty = 100;
        }

        out[written].temp = temp;
        out[written].duty = duty;
        written++;
    }

    return written;
}

void lg_curve_set_points(lg_curve *curve, const lg_point *points, size_t n)
{
    if (curve == NULL) {
        return;
    }
    curve->npoints = lg_curve_normalize(points, n, curve->points, LG_CURVE_MAX_POINTS);
    curve->samples_valid = false;
}

/*
 * Bezier control points live in double precision. Storing them in the integer
 * lg_point type truncates the fractional tangent offsets and visibly changes
 * the rendered curve, so a dedicated double-precision point type is required.
 */
typedef struct {
    double x;
    double y;
} lg_dpoint;

static double bezier_axis(double p0, double p1, double p2, double p3, double t)
{
    double inv = 1.0 - t;
    double inv2 = inv * inv;
    double t2 = t * t;
    return (inv2 * inv * p0)
         + (3.0 * inv2 * t * p1)
         + (3.0 * inv * t2 * p2)
         + (t2 * t * p3);
}

/* Cubic Bezier sample with the y axis clamped to 0..100, as the Python did. */
static void bezier_point(const lg_dpoint *p0, const lg_dpoint *p1, const lg_dpoint *p2,
                         const lg_dpoint *p3, double t, double *out_x, double *out_y)
{
    *out_x = bezier_axis(p0->x, p1->x, p2->x, p3->x, t);
    double y = bezier_axis(p0->y, p1->y, p2->y, p3->y, t);
    if (y < 0.0) {
        y = 0.0;
    } else if (y > 100.0) {
        y = 100.0;
    }
    *out_y = y;
}

size_t lg_curve_build_samples(lg_curve *curve)
{
    if (curve == NULL) {
        return 0;
    }
    if (curve->samples_valid) {
        return curve->nsamples;
    }

    curve->nsamples = 0;
    size_t n = curve->npoints;

    if (n == 0) {
        return 0;
    }
    if (n > LG_CURVE_MAX_POINTS) {
        n = LG_CURVE_MAX_POINTS;
    }
    if (n == 1) {
        curve->sx[0] = curve->points[0].temp;
        curve->sy[0] = curve->points[0].duty;
        curve->nsamples = 1;
        curve->samples_valid = true;
        return 1;
    }

    const lg_point *pts = curve->points;
    size_t out = 0;
    curve->sx[out] = pts[0].temp;
    curve->sy[out] = pts[0].duty;
    out++;

    for (size_t i = 0; i + 1 < n && out < LG_CURVE_MAX_SAMPLES; i++) {
        const lg_point *p0 = (i > 0) ? &pts[i - 1] : &pts[i];
        const lg_point *p1 = &pts[i];
        const lg_point *p2 = &pts[i + 1];
        const lg_point *p3 = (i + 2 < n) ? &pts[i + 2] : &pts[i + 1];

        /* Catmull-Rom style tangents scaled to Bezier control points, kept in
         * double precision because the offsets are fractional. */
        lg_dpoint b0 = {(double)p1->temp, (double)p1->duty};
        lg_dpoint b1 = {
            p1->temp + ((double)(p2->temp - p0->temp) / 6.0),
            p1->duty + ((double)(p2->duty - p0->duty) / 6.0),
        };
        lg_dpoint b2 = {
            p2->temp - ((double)(p3->temp - p1->temp) / 6.0),
            p2->duty - ((double)(p3->duty - p1->duty) / 6.0),
        };
        lg_dpoint b3 = {(double)p2->temp, (double)p2->duty};

        for (int step = 1; step <= LG_CURVE_SAMPLES_PER_SEGMENT; step++) {
            if (out >= LG_CURVE_MAX_SAMPLES) {
                break;
            }
            double t = (double)step / (double)LG_CURVE_SAMPLES_PER_SEGMENT;
            bezier_point(&b0, &b1, &b2, &b3, t, &curve->sx[out], &curve->sy[out]);
            out++;
        }
    }

    curve->nsamples = out;
    curve->samples_valid = true;
    return out;
}

/* Cast a Python-style rounded double to int, clamping to the 0..100 range. */
static int duty_from_sample(double v)
{
    if (v < 0.0) {
        v = 0.0;
    } else if (v > 100.0) {
        v = 100.0;
    }
    return (int)lg_py_round(v);
}

static int duty_from_samples(const double *sx, const double *sy, size_t n, double temp_c)
{
    if (n == 0) {
        return 0;
    }
    if (temp_c <= sx[0]) {
        return duty_from_sample(sy[0]);
    }
    if (temp_c >= sx[n - 1]) {
        return duty_from_sample(sy[n - 1]);
    }

    double best = sy[n - 1];

    for (size_t i = 0; i + 1 < n; i++) {
        double left = sx[i];
        double right = sx[i + 1];
        double lo = (left < right) ? left : right;
        double hi = (left < right) ? right : left;

        if (lo <= temp_c && temp_c <= hi) {
            double span = right - left;
            if (fabs(span) < 1e-9) {
                /* Degenerate segment: remember it and keep scanning. */
                best = (sy[i] > sy[i + 1]) ? sy[i] : sy[i + 1];
                continue;
            }
            double ratio = (temp_c - left) / span;
            double yl = sy[i];
            double yr = sy[i + 1];
            return duty_from_sample(yl + ((yr - yl) * ratio));
        }
    }

    return duty_from_sample(best);
}

int lg_curve_duty(const lg_curve *curve, double temp_c)
{
    if (curve == NULL) {
        return 0;
    }

    lg_curve *mutable_curve = (lg_curve *)curve;
    if (lg_curve_build_samples(mutable_curve) == 0) {
        return 0;
    }

    return duty_from_samples(curve->sx, curve->sy, curve->nsamples, temp_c);
}

int lg_curve_duty_points(const lg_point *points, size_t n, double temp_c)
{
    lg_curve scratch;
    lg_curve_init(&scratch);
    lg_curve_set_points(&scratch, points, n);
    return lg_curve_duty(&scratch, temp_c);
}
