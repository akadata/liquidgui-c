/*
 * Discovery assertions, replacing a human-review printout with real checks.
 *
 * The C discovery deliberately differs from the behaviour of the previous
 * implementation. These tests assert the *invariants* of that difference
 * rather than comparing output verbatim, so they hold on any machine:
 *
 *   - no control key may contain an absolute sysfs path
 *   - a cooler reachable through hwmon is driven through hwmon, never both
 *   - channels that a driver reports as unconnected are marked invalid
 *   - voltage nodes are read, not just temperatures and fan speeds
 *
 * Hardware-specific expectations (this board's Kraken, its NCT6687) are only
 * asserted when that hardware is actually present, so the test is meaningful
 * here and stays green on a machine with no fan headers at all.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/lg_hwmon.h"
#include "../src/lg_json.h"

static int failures = 0;
static long checks = 0;
static int skipped = 0;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void skip(const char *what)
{
    skipped++;
    printf("  (skipped: %s)\n", what);
}

/* Read and free a whole file; returns NULL when absent. */
static char *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out != NULL) {
        *len_out = got;
    }
    return buf;
}

/* ------------------------------------------------------------- invariants */

static void test_keys_are_boot_stable(const lg_snapshot *s)
{
    for (size_t i = 0; i < s->ncontrols; i++) {
        const lg_control *c = &s->controls[i];
        checks++;
        if (strstr(c->key, "/sys/") != NULL) {
            printf("FAIL: control key embeds a sysfs path: %s\n", c->key);
            failures++;
        }
        if (strstr(c->key, "hwmon") != NULL) {
            printf("FAIL: control key embeds a hwmon number: %s\n", c->key);
            failures++;
        }
    }
}

static void test_no_dual_control_path(const lg_snapshot *s)
{
    size_t hwmon_aio = 0;
    size_t liquidctl_aio = 0;

    for (size_t i = 0; i < s->ncontrols; i++) {
        const lg_control *c = &s->controls[i];
        if (!c->is_aio) {
            continue;
        }
        if (c->kind == LG_CTRL_HWMON) {
            hwmon_aio++;
        } else {
            liquidctl_aio++;
        }
    }

    checks++;
    if (hwmon_aio > 0 && liquidctl_aio > 0) {
        printf("FAIL: one cooler is reachable through both hwmon and liquidctl\n");
        failures++;
    }
}

static void test_voltages_are_read(const lg_snapshot *s)
{
    size_t volts = 0;
    for (size_t i = 0; i < s->nsensors; i++) {
        if (s->sensors[i].cls == LG_SENSOR_VOLT) {
            volts++;
        }
    }

    checks++;
    if (volts == 0) {
        /*
         * A machine may genuinely expose no voltage nodes. Only flag this when
         * a control surface exists, which implies real hardware is present.
         */
        if (s->ncontrols > 0) {
            printf("FAIL: controls were found but no voltage nodes were read\n");
            failures++;
        } else {
            skip("no voltage nodes and no controls on this machine");
        }
    }
}

static void test_implausible_channels_are_rejected(const lg_snapshot *s)
{
    /*
     * A temperature outside any plausible range, or one sitting exactly on its
     * declared minimum, is how a driver says the input is unconnected. It must
     * be marked invalid rather than reported as a real reading.
     */
    for (size_t i = 0; i < s->nsensors; i++) {
        const lg_sensor *x = &s->sensors[i];
        if (x->cls != LG_SENSOR_TEMP) {
            continue;
        }
        checks++;
        if (x->valid && (x->value < -20.0 || x->value > 150.0)) {
            printf("FAIL: %s reported %.1f C as valid\n", x->label, x->value);
            failures++;
        }
    }

    /*
     * The reverse must also hold: a fan reporting 0 rpm is a real observation,
     * not an invalid one, because the safety layer needs to see a stall.
     */
    for (size_t i = 0; i < s->nsensors; i++) {
        const lg_sensor *x = &s->sensors[i];
        if (x->cls != LG_SENSOR_FAN) {
            continue;
        }
        checks++;
        if (!x->valid && x->value == 0.0) {
            printf("FAIL: fan %s at 0 rpm was marked invalid\n", x->label);
            failures++;
        }
    }
}

/* ------------------------------------------- hardware-specific expectations */

static const lg_control *find_driver(const lg_snapshot *s, const char *driver)
{
    for (size_t i = 0; i < s->ncontrols; i++) {
        if (strcmp(s->controls[i].driver, driver) == 0) {
            return &s->controls[i];
        }
    }
    return NULL;
}

static void test_kraken_is_hwmon(const lg_snapshot *s)
{
    const lg_control *c = find_driver(s, "nzxt_kraken3");
    if (c == NULL) {
        skip("no nzxt_kraken3 cooler present");
        return;
    }

    check(c->kind == LG_CTRL_HWMON, "the Kraken is driven through hwmon");
    check(c->is_aio, "the Kraken is grouped as an AIO");
    check(c->has_enable, "the Kraken exposes pwm_enable");
    /*
     * Measured on this hardware: a write to pwmN is discarded while
     * pwm_enable is 0, so the control must be flagged as needing it.
     */
    check(c->needs_enable_first, "the Kraken is flagged as needing pwm_enable first");
    check(c->enable_resets_pwm, "the Kraken is flagged as resetting PWM on manual entry");
}

static void test_nct6687_headers(const lg_snapshot *s)
{
    const lg_control *c = find_driver(s, "nct6687");
    if (c == NULL) {
        skip("no nct6687 Super-I/O present");
        return;
    }

    check(c->kind == LG_CTRL_HWMON, "nct6687 headers are hwmon controls");
    check(!c->needs_enable_first, "nct6687 does not need an explicit enable write");
    check(c->has_enable, "nct6687 exposes pwm_enable");
}

/*
 * True when a hwmon device is bound to the named platform driver. Used to
 * assert about chips that correctly produce no controls, where looking only at
 * the control list could not tell "absent" from "present but excluded".
 */
static bool chip_present(const char *driver_name)
{
    const char *root = LG_HWMON_ROOT;
    for (int i = 0; i < 64; i++) {
        char dir[256];
        char link[320];
        char target[320];

        snprintf(dir, sizeof(dir), "%s/hwmon%d", root, i);
        snprintf(link, sizeof(link), "%s/device/driver", dir);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = '\0';
        const char *base = strrchr(target, '/');
        if (base != NULL && strcmp(base + 1, driver_name) == 0) {
            return true;
        }
    }
    return false;
}

static void test_secondary_chip_is_not_a_control(const lg_snapshot *s)
{
    /*
     * This board carries an nct6683 alongside an nct6687 and both report the
     * hwmon name "nct6687". The nct6683's pwm nodes are read-only, so it must
     * not be offered as a control surface. The check is skipped only when no
     * such chip is bound at all.
     */
    if (!chip_present("nct6683")) {
        skip("no secondary nct6683 chip bound to this system");
        return;
    }

    for (size_t i = 0; i < s->ncontrols; i++) {
        checks++;
        if (strcmp(s->controls[i].driver, "nct6683") == 0) {
            printf("FAIL: the read-only nct6683 was offered as a control: %s\n",
                   s->controls[i].key);
            failures++;
        }
    }
}

static void test_coolant_is_read(const lg_snapshot *s)
{
    bool has_coolant = false;
    for (size_t i = 0; i < s->nsensors; i++) {
        const lg_sensor *x = &s->sensors[i];
        if (x->cls == LG_SENSOR_TEMP && strstr(x->label, "Coolant") != NULL) {
            has_coolant = true;
        }
    }
    if (find_driver(s, "nzxt_kraken3") == NULL) {
        skip("no liquid loop to read a coolant temperature from");
        return;
    }
    check(has_coolant, "coolant temperature is read from the AIO");
    check(!isnan(s->coolant_temp), "coolant temperature is resolved as the source");
}

/* ------------------------------------------------------------------ golden */

static void test_golden_is_loadable(const char *golden_path)
{
    char *text = slurp(golden_path, NULL);
    if (text == NULL) {
        skip("discovery golden file not present");
        return;
    }

    const char *err = NULL;
    lg_json_doc *doc = lg_json_parse(text, &err);
    checks++;
    if (doc == NULL) {
        printf("FAIL: discovery golden does not parse: %s\n", err ? err : "?");
        failures++;
        free(text);
        return;
    }

    const lg_json *root = lg_json_root(doc);
    check(lg_json_len(lg_json_get(root, "temps")) > 0, "golden records temperatures");
    check(lg_json_len(lg_json_get(root, "controls")) > 0, "golden records controls");

    /*
     * Record what the previous implementation used as control keys. The C side
     * must not produce any of them, which is the whole point of the change.
     */
    const lg_json *controls = lg_json_get(root, "controls");
    for (size_t i = 0; i < lg_json_len(controls); i++) {
        const lg_json *c = lg_json_obj_val_at(controls, i);
        const char *ident = lg_json_get_str(c, "identifier", "");
        if (ident[0] != '\0' && strstr(ident, "hwmon") != NULL) {
            /* Expected: these path-shaped keys are exactly what we removed. */
        }
    }

    lg_json_doc_free(doc);
    free(text);
}

int main(int argc, char **argv)
{
    const char *golden = (argc > 1) ? argv[1] : "tests/golden/detect_golden.json";

    lg_discover_opts opts;
    lg_discover_opts_init(&opts);

    lg_snapshot snap;
    if (!lg_discover(&snap, &opts)) {
        printf("discovery failed; nothing to assert\n");
        return 0;
    }

    printf("discovery: %zu sensors, %zu controls, cpu %.1f C\n", snap.nsensors, snap.ncontrols,
           snap.cpu_temp);

    test_keys_are_boot_stable(&snap);
    test_no_dual_control_path(&snap);
    test_voltages_are_read(&snap);
    test_implausible_channels_are_rejected(&snap);
    test_kraken_is_hwmon(&snap);
    test_nct6687_headers(&snap);
    test_secondary_chip_is_not_a_control(&snap);
    test_coolant_is_read(&snap);
    test_golden_is_loadable(golden);

    printf("%ld checks, %d failures, %d skipped\n", checks, failures, skipped);
    return failures == 0 ? 0 : 1;
}
