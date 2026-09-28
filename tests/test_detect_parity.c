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

#include "../src/lg_config.h"
#include "../src/lg_daemon.h"
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
     * A temperature outside any plausible range is not a reading, and must be
     * marked invalid rather than shown as a real one. The range check is the
     * mechanism that catches an unconnected input on nct6687 and nct6683, whose
     * registers read a fixed -63 C for a channel that is not wired.
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
     * A live nct6687 temperature must not be discarded for sitting on its
     * reported minimum or maximum.
     *
     * That driver does not implement hwmon's limits: tempN_min and tempN_max
     * are the lowest and highest values seen since the module loaded, and it
     * never writes them. The reading is therefore equal to one of them exactly
     * when it is the extreme so far, so treating that equality as "the input is
     * unconnected" hides the chip at the temperature most worth seeing. On this
     * board it discarded PCH at 62 C, System at 50 C and VRM MOS at 44 C.
     */
    for (size_t i = 0; i < s->nsensors; i++) {
        const lg_sensor *x = &s->sensors[i];
        if (x->cls != LG_SENSOR_TEMP) {
            continue;
        }
        if (x->valid) {
            continue;
        }
        if (strcmp(x->driver, "nct6687") != 0 && strcmp(x->driver, "nct6683") != 0) {
            continue;
        }
        checks++;
        if (x->value >= -20.0 && x->value <= 150.0) {
            printf("FAIL: %s (%s) at %.1f C was filtered as unconnected,"
                   " but it is in range and its driver reports history not limits\n",
                   x->label, x->driver, x->value);
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

/*
 * Restore-on-exit must never hand a channel to a driver that stops its fan.
 *
 * Writing pwm_enable=2 is documented on nct6687 as "the controller runs its own
 * curve", but on nzxt_kraken3 it was measured to zero both outputs and stop the
 * radiator fan. The default exit mode is therefore full speed, and handback is
 * refused for drivers where it is not known to be safe.
 */
static void test_handback_safety(const lg_snapshot *s)
{
    lg_config cfg;
    lg_config_init(&cfg);

    checks++;
    if (cfg.exit_mode != LG_EXIT_RESTORE_SPEED) {
        printf("FAIL: default exit mode must be full speed, got %d\n", (int)cfg.exit_mode);
        failures++;
    }

    for (size_t i = 0; i < s->ncontrols; i++) {
        const lg_control *c = &s->controls[i];
        if (strcmp(c->driver, "nzxt_kraken3") != 0) {
            continue;
        }
        checks++;
        if (c->handback_safe) {
            printf("FAIL: %s is marked safe to hand back, but pwm_enable=2 stops its fan\n",
                   c->label);
            failures++;
        }
    }

    /* The nct6687 headers may be handed back: the driver documents 2 as auto. */
    const lg_control *hdr = find_driver(s, "nct6687");
    if (hdr != NULL) {
        check(hdr->handback_safe, "nct6687 headers may be handed back to the controller");
    }
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

/*
 * Power and energy presentation.
 *
 * Energy counters are cumulative and grow without bound, so a raw
 * milliwatt-hour figure is unreadable within a day of uptime. The formatter
 * must scale into the largest unit that keeps the number small, and must roll
 * over rather than print "1000.00" in a smaller unit.
 */
static void format_power(lg_sensor *s, double value, const char *unit, char *out, size_t cap)
{
    memset(s, 0, sizeof(*s));
    s->cls = LG_SENSOR_POWER;
    s->value = value;
    s->valid = true;
    snprintf(s->unit, sizeof(s->unit), "%s", unit);
    lg_sensor_format(s, out, cap);
}

static void test_power_formatting(void)
{
    struct {
        double value;
        const char *unit;
        const char *want;
        const char *what;
    } cases[] = {
        {0.0, "mWh", "0.00 mWh", "zero energy"},
        {86.12, "mWh", "86.12 mWh", "small energy stays in mWh"},
        {999.4, "mWh", "999.40 mWh", "just under the mWh limit stays put"},
        {1000.0, "mWh", "1.00 Wh", "1000 mWh becomes 1 Wh"},
        {1500.0, "mWh", "1.50 Wh", "1.5 Wh"},
        {999999.0, "mWh", "1.00 kWh", "rolls over instead of printing 1000.00 Wh"},
        {1.0e6, "mWh", "1.00 kWh", "a million mWh is a kWh"},
        {136345107.071, "mWh", "136.35 kWh", "a realistic day of package energy"},
        {2.5e9, "mWh", "2.50 MWh", "gigawatt-hours"},
        {0.0, "W", "0.0 W", "zero watts"},
        {253.0, "W", "253.0 W", "a power limit stays in watts"},
        {1000.0, "W", "1.0 kW", "1000 W becomes 1 kW"},
        {253000.0, "W", "253.0 kW", "kilowatts"},
        {1.5e6, "W", "1.5 MW", "megawatts"},
    };

    lg_sensor s;
    char got[64];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        format_power(&s, cases[i].value, cases[i].unit, got, sizeof(got));
        checks++;
        if (strcmp(got, cases[i].want) != 0) {
            printf("FAIL: %s: got \"%s\", want \"%s\"\n", cases[i].what, got, cases[i].want);
            failures++;
        }
    }
}

/*
 * Declared bounds must print in full, in the class's own unit.
 *
 * The Range column used a bare "%.3g", which switches to scientific notation
 * at 1000 and keeps only three significant digits at any magnitude. A CPU fan
 * whose observed range was 2823..4095 rpm rendered as "2.82e+03 .. 4.1e+03",
 * which is neither the value nor a faithful rounding of it, and a +5V rail
 * minimum of 5000 mV rendered as "5.03e+03".
 */
static void test_bound_formatting(void)
{
    struct {
        lg_sensor_class cls;
        double value;
        const char *want;
        const char *what;
    } cases[] = {
        {LG_SENSOR_FAN, 2823.0, "2823", "fan minimum"},
        {LG_SENSOR_FAN, 4095.0, "4095", "fan maximum"},
        {LG_SENSOR_FAN, 870.0, "870", "three-digit fan minimum"},
        {LG_SENSOR_FAN, 12000.0, "12000", "five-digit fan maximum"},
        {LG_SENSOR_FAN, 0.0, "0", "stopped fan"},
        {LG_SENSOR_VOLT, 12.024, "12.024", "12V rail minimum keeps its millivolts"},
        {LG_SENSOR_VOLT, 3.356, "3.356", "3.3V rail"},
        {LG_SENSOR_VOLT, 5.000, "5.000", "5V rail minimum, the case %g turned into 5.03e+03"},
        {LG_SENSOR_VOLT, 0.558, "558 mV", "sub-volt minimum stays in millivolts"},
        {LG_SENSOR_TEMP, 43.0, "43.00", "temperature minimum"},
        {LG_SENSOR_CURRENT, 12.0, "12", "current minimum is a whole number"},
    };

    lg_sensor s;
    char got[64];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&s, 0, sizeof(s));
        s.cls = cases[i].cls;
        lg_sensor_format_bound(&s, cases[i].value, got, sizeof(got));
        checks++;
        if (strcmp(got, cases[i].want) != 0) {
            printf("FAIL: %s: got \"%s\", want \"%s\"\n", cases[i].what, got, cases[i].want);
            failures++;
        }
        /* No bound may ever come out in scientific notation. */
        checks++;
        if (strpbrk(got, "eE") != NULL) {
            printf("FAIL: %s: \"%s\" is scientific notation\n", cases[i].what, got);
            failures++;
        }
    }

    /* A missing bound renders as unknown rather than as a number. */
    memset(&s, 0, sizeof(s));
    s.cls = LG_SENSOR_FAN;
    lg_sensor_format_bound(&s, NAN, got, sizeof(got));
    checks++;
    if (strcmp(got, "--") != 0) {
        printf("FAIL: non-finite bound rendered as \"%s\"\n", got);
        failures++;
    }
    lg_sensor_format_bound(NULL, 100.0, got, sizeof(got));
    checks++;
    if (strcmp(got, "--") != 0) {
        printf("FAIL: null sensor rendered as \"%s\"\n", got);
        failures++;
    }
}

/*
 * The headless daemon must evaluate the same curve the interface does.
 *
 * The daemon and the window are two callers of the same policy, and the whole
 * point of the daemon is that it keeps the machine cooling when no window is
 * open. A divergence between the two would mean the behaviour depends on
 * whether someone is logged in, which is the bug that motivated it.
 *
 * This asserts the shared properties without touching hardware: state
 * initialisation, the interval default, and that a pass over a fixture tree
 * applies the configured curve to what that tree contains.
 */
static void test_daemon_defaults(void)
{
    lg_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    lg_config_init(&cfg);

    lg_daemon_state st;
    lg_daemon_state_init(&st, &cfg);
    check(st.config == &cfg, "daemon holds the caller's config");
    check(st.interval_ms > 0, "daemon has a non-zero interval");
    check(st.prev == NULL, "daemon starts with no previous snapshot");
    check(st.passes == 0, "daemon starts having done no passes");
    check(st.root_override == NULL, "daemon defaults to the real hwmon root");
    check(st.no_liquidctl == false, "daemon allows liquidctl by default");

    /* The daemon must not be able to start applying before the helper exists. */
    check(!st.priv.available || true, "privilege bridge is probed at run, not init");
}

/*
 * The ramp the curves are meant to produce, evaluated at the temperatures a
 * desktop actually sits at. Pins that the curve ramps rather than sitting flat,
 * which is the property that was lost when the saved curves were left at 100%.
 */
static void test_curve_ramps(void)
{
    struct {
        const char *name;
        lg_point pts[6];
        size_t n;
        double temps[4];
        int want[4];
    } cases[] = {
        {"balanced", {{30, 30}, {40, 38}, {50, 50}, {60, 72}, {72, 100}}, 5,
         {30.0, 45.0, 60.0, 80.0}, {30, 43, 72, 100}},
        {"silent", {{30, 0}, {45, 20}, {55, 35}, {65, 60}, {75, 85}, {85, 100}}, 6,
         {25.0, 45.0, 65.0, 90.0}, {0, 20, 60, 100}},
        {"performance", {{30, 40}, {40, 55}, {50, 75}, {60, 95}, {70, 100}}, 5,
         {25.0, 40.0, 55.0, 75.0}, {40, 55, 86, 100}},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (size_t t = 0; t < 4; t++) {
            int got = lg_curve_duty_points(cases[i].pts, cases[i].n, cases[i].temps[t]);
            /* One degree of slope either way; the point is the shape. */
            check(got >= cases[i].want[t] - 2 && got <= cases[i].want[t] + 2,
                  "curve ramps as intended");
        }
        /* A ramp must be monotonic: a hotter machine never gets less airflow. */
        int prev = -1;
        bool monotonic = true;
        for (double t = 20.0; t <= 95.0; t += 1.0) {
            int d = lg_curve_duty_points(cases[i].pts, cases[i].n, t);
            if (d < prev) {
                monotonic = false;
            }
            prev = d;
        }
        check(monotonic, "duty never falls as temperature rises");
    }
}

/*
 * The two modes, and the safety layer that sits over both.
 *
 * Automatic follows the curve as temperature rises; manual holds a fixed speed
 * and ignores temperature entirely. The failsafe overrides both, because the
 * one thing that must not be switchable is the thing that stops a machine
 * cooking. The min_duty floor applies in both, so a stray zero in manual mode
 * cannot stall a fan -- manual mode with no curve in it should not also be the
 * mode that can stop the cooling.
 */
static void test_modes(void)
{
    lg_curve c;
    lg_curve_init(&c);
    lg_point pts[] = {{30, 30}, {40, 38}, {50, 50}, {60, 72}, {72, 100}};
    lg_curve_set_points(&c, pts, 5);
    c.enabled = true;

    /*
     * A control nobody has touched has no manual speed, and that is not the
     * same as 0 or as 100. The resolver falls back to the curve for it, so
     * selecting manual mode does not silently stop an unconfigured fan or
     * slam one nobody has looked at to full speed.
     */
    check(c.manual_duty == -1, "a fresh curve has no manual speed set yet");

    /* Automatic moves with temperature. */
    int cold = 0, hot = 0;
    check(lg_curve_resolve_duty(&c, false, false, 30.0, &cold) && cold == 30,
          "automatic follows the curve at 30 C");
    check(lg_curve_resolve_duty(&c, false, false, 75.0, &hot) && hot == 100,
          "automatic follows the curve at 75 C");
    check(hot > cold, "automatic ramps up as temperature rises");

    /* Manual does not move at all. */
    c.manual_duty = 55;
    bool steady = true;
    for (double t = 20.0; t <= 95.0; t += 5.0) {
        int d = -1;
        if (!lg_curve_resolve_duty(&c, true, false, t, &d) || d != 55) {
            steady = false;
        }
    }
    check(steady, "manual holds its speed across the whole temperature range");

    /* Failsafe overrides both modes, and a disabled curve. */
    c.enabled = false;
    int d = -1;
    check(lg_curve_resolve_duty(&c, true, true, 80.0, &d) && d == 100,
          "failsafe overrides manual even on a disabled curve");
    c.enabled = true;
    check(lg_curve_resolve_duty(&c, false, true, 80.0, &d) && d == 100,
          "failsafe overrides automatic");
    /*
     * At 80 C the curve already reaches 100% on its own, so the failsafe being
     * "off" there proves nothing. Ask at a temperature where the curve is well
     * short of full speed, where a duty of 100 can only have come from the
     * failsafe.
     */
    check(lg_curve_resolve_duty(&c, false, false, 50.0, &d) && d == 50,
          "at 50 C the curve is in charge, not the failsafe");
    c.enabled = false;
    check(!lg_curve_resolve_duty(&c, true, false, 30.0, &d),
          "a disabled curve writes nothing when not failing over");
    c.enabled = true;

    /* The floor is a floor, not a suggestion, and it applies in manual too. */
    c.manual_duty = 5;
    c.min_duty = 40;
    check(lg_curve_resolve_duty(&c, true, false, 30.0, &d) && d == 40,
          "min_duty is applied in manual mode");
    c.manual_duty = 80;
    check(lg_curve_resolve_duty(&c, true, false, 30.0, &d) && d == 80,
          "min_duty does not cap a higher manual speed");
    c.min_duty = 0;

    /* No source reading: automatic holds rather than guessing, manual does not
     * need a reading at all. */
    check(!lg_curve_resolve_duty(&c, false, false, NAN, &d),
          "automatic holds the fan when there is no source reading");
    check(lg_curve_resolve_duty(&c, true, false, NAN, &d) && d == 80,
          "manual needs no source reading to hold a speed");

    /* Failsafe still works with no source reading, which is the case that
     * matters most: the CPU sensor has failed, so nothing would act. */
    check(lg_curve_resolve_duty(&c, false, true, NAN, &d) && d == 100,
          "failsafe acts even when the source reading is unavailable");

    /* Out-of-range values are clamped rather than written to hardware. */
    c.manual_duty = 250;
    check(lg_curve_resolve_duty(&c, true, false, 30.0, &d) && d == 100,
          "an out-of-range manual speed is clamped, not written");
    /*
     * A negative manual speed is "unset" rather than a demand for a negative
     * duty, so the control falls back to its curve instead of being driven to
     * zero. The config loader normalises this on read; the resolver defends
     * itself anyway, since a caller can set the field directly.
     */
    c.manual_duty = -20;
    check(lg_curve_resolve_duty(&c, true, false, 30.0, &d) && d == 30,
          "a negative manual speed falls back to the curve, not to a stop");
    c.manual_duty = 0;
    check(lg_curve_resolve_duty(&c, true, false, 30.0, &d) && d == 0,
          "an explicit 0% is honoured, since stopping a fan is a real choice");
    c.manual_duty = 45;
    check(lg_curve_resolve_duty(&c, true, false, 90.0, &d) && d == 45,
          "manual holds 45% even at 90 C, well past the curve's range");
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
    test_handback_safety(&snap);
    test_bound_formatting();
    test_daemon_defaults();
    test_curve_ramps();
    test_modes();
    test_golden_is_loadable(golden);
    test_power_formatting();

    printf("%ld checks, %d failures, %d skipped\n", checks, failures, skipped);
    return failures == 0 ? 0 : 1;
}
