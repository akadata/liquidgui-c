/*
 * Curve parity test: compare lg_curve against tests/golden/curve_golden.json,
 * which was captured from the legacy Python implementation.
 *
 * Any mismatch here means the C rewrite would change cooling behaviour, so the
 * test compares the full float sample table and every duty lookup.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lg_curve.h"
#include "../src/lg_json.h"

static int failures = 0;
static long checks = 0;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    failures++;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len != NULL) {
        *out_len = got;
    }
    return buf;
}

int main(int argc, char **argv)
{
    const char *golden_path = (argc > 1) ? argv[1] : "tests/golden/curve_golden.json";

    char *text = read_file(golden_path, NULL);
    if (text == NULL) {
        fprintf(stderr, "cannot read golden file: %s\n", golden_path);
        return 2;
    }

    const char *err = NULL;
    lg_json_doc *doc = lg_json_parse(text, &err);
    free(text);
    if (doc == NULL) {
        fprintf(stderr, "cannot parse golden file: %s\n", err ? err : "?");
        return 2;
    }

    const lg_json *root = lg_json_root(doc);
    const lg_json *curves = lg_json_get(root, "curves");
    const lg_json *lookup = lg_json_get(root, "lookup_temps_c");

    if (lg_json_type_of(curves) != LG_JSON_OBJ || lg_json_type_of(lookup) != LG_JSON_ARR) {
        fprintf(stderr, "golden file has unexpected shape\n");
        lg_json_doc_free(doc);
        return 2;
    }

    size_t ncurves = lg_json_len(curves);
    printf("parity: %zu curves, %zu lookup temps\n", ncurves, lg_json_len(lookup));
    if (ncurves == 0) {
        fprintf(stderr, "golden has no curves\n");
        lg_json_doc_free(doc);
        return 2;
    }

    for (size_t ci = 0; ci < ncurves; ci++) {
        const char *key = lg_json_obj_key_at(curves, ci);
        const lg_json *entry = lg_json_obj_val_at(curves, ci);
        if (key == NULL) {
            continue;
        }

        const lg_json *jpoints = lg_json_get(entry, "points");
        const lg_json *jsamples = lg_json_get(entry, "samples");
        const lg_json *jduty = lg_json_get(entry, "duty");

        /* Rebuild the curve from the golden's *normalized* points. */
        lg_curve curve;
        lg_curve_init(&curve);

        size_t np = lg_json_len(jpoints);
        if (np == 0 || np > LG_CURVE_MAX_POINTS) {
            fail("%s: bad point count %zu", key, np);
            continue;
        }

        lg_point pts[LG_CURVE_MAX_POINTS];
        for (size_t i = 0; i < np; i++) {
            const lg_json *pair = lg_json_at(jpoints, i);
            pts[i].temp = (int)lg_json_as_num(lg_json_at(pair, 0), 0);
            pts[i].duty = (int)lg_json_as_num(lg_json_at(pair, 1), 0);
        }
        lg_curve_set_points(&curve, pts, np);

        if (curve.npoints != np) {
            fail("%s: normalized point count %zu != golden %zu", key, curve.npoints, np);
        }

        /* 1. Full float sample table. */
        size_t ns = lg_curve_build_samples(&curve);
        size_t jns = lg_json_len(jsamples);
        checks++;

        if (ns != jns) {
            fail("%s: sample count %zu != golden %zu", key, ns, jns);
        } else {
            double worst = 0.0;
            int worst_idx = -1;
            for (size_t i = 0; i < ns; i++) {
                const lg_json *pair = lg_json_at(jsamples, i);
                double gx = lg_json_as_num(lg_json_at(pair, 0), 0.0);
                double gy = lg_json_as_num(lg_json_at(pair, 1), 0.0);
                double dx = fabs(curve.sx[i] - gx);
                double dy = fabs(curve.sy[i] - gy);
                if (dx > worst) {
                    worst = dx;
                    worst_idx = (int)i;
                }
                if (dy > worst) {
                    worst = dy;
                    worst_idx = (int)i;
                }
            }
            /* Golden values were rounded to 9dp, so 1e-9 is the floor. */
            if (worst > 1.5e-9) {
                fail("%s: sample[%d] deviates by %.3e", key, worst_idx, worst);
            }
        }

        /* 2. Every duty lookup. */
        for (size_t ti = 0; ti < lg_json_len(lookup); ti++) {
            double temp = lg_json_as_num(lg_json_at(lookup, ti), 0.0);

            /* Golden keys are the Python repr of the temperature, e.g. "42.0". */
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f", temp);
            int gold = (int)lg_json_as_num(lg_json_get(jduty, buf), -999);
            if (gold == -999) {
                fail("%s: no golden duty entry for key \"%s\"", key, buf);
                continue;
            }

            int got = lg_curve_duty(&curve, temp);
            checks++;
            if (got != gold) {
                fail("%s: duty(%.2f C) = %d, golden %d", key, temp, got, gold);
                if (failures > 20) {
                    fprintf(stderr, "too many failures, stopping\n");
                    lg_json_doc_free(doc);
                    return 1;
                }
            }
        }
    }

    lg_json_doc_free(doc);

    printf("%ld comparisons, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
