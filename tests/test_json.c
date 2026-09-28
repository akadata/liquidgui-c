/*
 * JSON reader/writer tests.
 *
 * The NaN case is here because it was a real defect: coolant temperature is NaN
 * whenever no liquid sensor is present, and the writer emitted a bare "nan",
 * which is not valid JSON and broke every consumer of --dump-detect on any
 * machine without an AIO coolant probe.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lg_json.h"

static int failures = 0;
static long checks = 0;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        printf("FAIL: %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
        failures++;
    }
}

/* Write a value and return the buffer the caller must free. */
static char *emit(lg_json_writer *w)
{
    return lg_json_writer_take(w);
}

static void test_numbers(void)
{
    struct {
        double in;
        const char *want;
        const char *what;
    } cases[] = {
        {0.0, "0", "zero"},
        {42.0, "42", "integral"},
        {-7.0, "-7", "negative integral"},
        {99.0, "99", "duty-like value"},
        {0.5, "0.5", "fractional"},
        {-0.25, "-0.25", "negative fractional"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lg_json_writer w;
        lg_json_writer_init(&w);
        lg_json_write_num(&w, cases[i].in);
        char *out = emit(&w);
        check_str(out, cases[i].want, cases[i].what);
        free(out);
    }

    /*
     * NaN and infinity have no JSON spelling. They must become null, not the
     * bare token that printf would produce.
     */
    const double specials[] = {NAN, INFINITY, -INFINITY};
    const char *names[] = {"NaN", "+infinity", "-infinity"};
    for (size_t i = 0; i < 3; i++) {
        lg_json_writer w;
        lg_json_writer_init(&w);
        lg_json_write_num(&w, specials[i]);
        char *out = emit(&w);
        check_str(out, "null", names[i]);
        free(out);
    }
}

static void test_escaping(void)
{
    struct {
        const char *in;
        const char *want;
        const char *what;
    } cases[] = {
        {"plain", "\"plain\"", "no special characters"},
        {"with \"quotes\"", "\"with \\\"quotes\\\"\"", "double quotes"},
        {"back\\slash", "\"back\\\\slash\"", "backslash"},
        {"line\nbreak", "\"line\\nbreak\"", "newline"},
        {"tab\there", "\"tab\\there\"", "tab"},
        {"ctrl\001char", "\"ctrl\\u0001char\"", "control character"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        lg_json_writer w;
        lg_json_writer_init(&w);
        lg_json_write_str(&w, cases[i].in);
        char *out = emit(&w);
        check_str(out, cases[i].want, cases[i].what);
        free(out);
    }
}

static void test_parse(void)
{
    const char *text =
        "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"e\"},\"f\":true,\"g\":null,\"h\":-1.5}";
    const char *err = NULL;
    lg_json_doc *doc = lg_json_parse(text, &err);
    check(doc != NULL, "well-formed document parses");
    if (doc == NULL) {
        return;
    }

    const lg_json *root = lg_json_root(doc);
    check(lg_json_get_num(root, "a", -1) == 1, "number member");
    check(lg_json_len(lg_json_get(root, "b")) == 3, "array length");
    check(lg_json_as_num(lg_json_at(lg_json_get(root, "b"), 1), -1) == 2, "array element");
    check_str(lg_json_get_str(lg_json_get(root, "c"), "d", ""), "e", "nested string");
    check(lg_json_get_bool(root, "f", false), "boolean member");
    check(lg_json_type_of(lg_json_get(root, "g")) == LG_JSON_NULL, "null member");
    check(lg_json_get_num(root, "h", 0) == -1.5, "negative float");
    check(lg_json_get_num(root, "missing", 42) == 42, "missing member uses the fallback");

    /* Iteration by position, which object lookup alone cannot do. */
    check(lg_json_len(root) == 6, "object member count");
    check_str(lg_json_obj_key_at(root, 0), "a", "first key by position");
    check(lg_json_as_num(lg_json_obj_val_at(root, 0), -1) == 1, "first value by position");
    check(lg_json_obj_key_at(root, 99) == NULL, "out-of-range key access is safe");
    check(lg_json_obj_val_at(root, 99) == NULL, "out-of-range value access is safe");

    lg_json_doc_free(doc);
}

static void test_parse_failures(void)
{
    static const char *bad[] = {
        "{",
        "{\"a\":}",
        "[1,2",
        "{\"a\":1}}",
        "{\"a\":1} trailing",
        "\"unterminated",
        "{\"a\":01x}",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        checks++;
        const char *err = NULL;
        lg_json_doc *doc = lg_json_parse(bad[i], &err);
        if (doc != NULL) {
            printf("FAIL: malformed input accepted: %s\n", bad[i]);
            failures++;
            lg_json_doc_free(doc);
        } else if (err == NULL) {
            printf("FAIL: no error description for: %s\n", bad[i]);
            failures++;
        }
    }

    /* Deep nesting must be refused rather than blowing the C stack. */
    checks++;
    char deep[4096];
    size_t n = 0;
    for (size_t i = 0; i < 2000 && n + 1 < sizeof(deep); i++) {
        deep[n++] = '[';
    }
    deep[n] = '\0';
    lg_json_doc *doc = lg_json_parse(deep, NULL);
    if (doc != NULL) {
        printf("FAIL: deep nesting accepted\n");
        failures++;
        lg_json_doc_free(doc);
    }
}

static void test_roundtrip(void)
{
    lg_json_writer w;
    lg_json_writer_init(&w);
    lg_json_begin_object(&w);
    lg_json_key(&w, "path");
    lg_json_write_str(&w, "/sys/class/hwmon/hwmon12/pwm1");
    lg_json_raw(&w, ",");
    lg_json_key(&w, "coolant");
    lg_json_write_num(&w, NAN);
    lg_json_raw(&w, ",");
    lg_json_key(&w, "points");
    lg_json_begin_array(&w);
    lg_json_begin_array(&w);
    lg_json_write_num(&w, 30);
    lg_json_raw(&w, ",");
    lg_json_write_num(&w, 70);
    lg_json_end_array(&w);
    lg_json_end_array(&w);
    lg_json_end_object(&w);

    char *text = emit(&w);
    check(text != NULL, "writer produces output");
    if (text == NULL) {
        return;
    }

    lg_json_doc *doc = lg_json_parse(text, NULL);
    check(doc != NULL, "emitted document parses back");

    if (doc != NULL) {
        const lg_json *root = lg_json_root(doc);
        check_str(lg_json_get_str(root, "path", ""), "/sys/class/hwmon/hwmon12/pwm1",
                  "string round-trips");
        check(lg_json_type_of(lg_json_get(root, "coolant")) == LG_JSON_NULL,
              "NaN round-trips as null");
        const lg_json *pt = lg_json_at(lg_json_get(root, "points"), 0);
        check(lg_json_as_num(lg_json_at(pt, 0), 0) == 30, "nested number round-trips");
        check(lg_json_as_num(lg_json_at(pt, 1), 0) == 70, "nested number round-trips");
        lg_json_doc_free(doc);
    }
    free(text);
}

int main(void)
{
    test_numbers();
    test_escaping();
    test_parse();
    test_parse_failures();
    test_roundtrip();

    printf("%ld checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
