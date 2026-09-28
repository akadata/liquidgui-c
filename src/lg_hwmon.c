/*
 * lg_hwmon - sensor and control discovery over /sys/class/hwmon.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_hwmon.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lg_liquidctl.h"

/* ------------------------------------------------------------- small utils */

void lg_discover_opts_init(lg_discover_opts *opts)
{
    if (opts == NULL) {
        return;
    }
    opts->root = LG_HWMON_ROOT;
    opts->use_liquidctl = true;
    opts->liquidctl_bin = "liquidctl";
}

/* Trim ASCII whitespace in place, returning the start of the trimmed text. */
static char *trim(char *s)
{
    if (s == NULL) {
        return NULL;
    }
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

/* Read a small sysfs text file. Returns true on success. */
static bool read_text_file(const char *path, char *out, size_t cap)
{
    if (path == NULL || out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';

    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return false;
    }

    size_t got = fread(out, 1, cap - 1, f);
    fclose(f);
    out[got] = '\0';
    return true;
}

/* Read an integer node. Returns false when absent or unparseable. */
static bool read_int_file(const char *path, long *out)
{
    char buf[64];
    if (!read_text_file(path, buf, sizeof(buf))) {
        return false;
    }
    char *t = trim(buf);
    if (t == NULL || *t == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    long v = strtol(t, &end, 10);
    if (end == t || errno != 0) {
        return false;
    }
    *out = v;
    return true;
}

/* Return true when the node exists and is writable by its owner. */
static bool node_is_writable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return (st.st_mode & S_IWUSR) != 0;
}

/*
 * Match a sysfs node name against "<prefix><digits><suffix>", e.g.
 * ("temp", "_input", "temp12_input") and ("in", "_input", "in0_input").
 * Returns the channel number, or -1 when the name does not match.
 *
 * This deliberately walks past the digits rather than computing an offset from
 * the parsed value: index 0 and multi-digit channels break that shortcut.
 */
static int match_channel(const char *name, const char *prefix, const char *suffix)
{
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) {
        return -1;
    }

    size_t i = plen;
    if (!isdigit((unsigned char)name[i])) {
        return -1;
    }

    long value = 0;
    while (isdigit((unsigned char)name[i])) {
        value = value * 10 + (name[i] - '0');
        i++;
    }

    if (strcmp(name + i, suffix) != 0) {
        return -1;
    }
    return (int)value;
}

/* Basename of a sysfs path, e.g. "temp12_input". */
static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    /* Manual bounded copy: snprintf("%s") makes the compiler warn about a
     * truncation it cannot rule out, and truncation here would silently
     * produce a wrong sysfs path. */
    size_t i = 0;
    while (i + 1 < cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void append_note(lg_snapshot *snap, const char *fmt, ...)
{
    if (snap == NULL) {
        return;
    }
    if (snap->notes[0] != '\0') {
        size_t n = strlen(snap->notes);
        if (n + 2 < sizeof(snap->notes)) {
            snap->notes[n] = '\n';
            snap->notes[n + 1] = '\0';
        }
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(snap->notes + strlen(snap->notes), sizeof(snap->notes) - strlen(snap->notes), fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- validity */

/*
 * Decide whether a reading is believable.
 *
 * The driver-declared bound check is applied to temperatures only. hwmon
 * drivers park an unconnected temperature input on its *_min sentinel, which
 * is how the NCT6687's "PCIe x1" (pinned at 193 C) and "Virtual 0" (-63 C)
 * are correctly identified as dead.
 *
 * That same rule must NOT be applied to voltages, currents or fans. A CPU
 * Vcore resting at its declared minimum of 558 mV is a perfectly normal idle
 * reading, and a fan reporting 0 rpm may be a real stall that the safety layer
 * needs to see rather than have hidden as "invalid".
 */
static bool reading_is_valid(lg_sensor *s)
{
    const double v = s->value;
    double lo = 0.0;
    double hi = 0.0;
    bool sentinel_check = false;

    switch (s->cls) {
    case LG_SENSOR_TEMP:
        lo = -20.0;
        hi = 150.0;
        sentinel_check = true;
        break;
    case LG_SENSOR_FAN:
        lo = -1.0;
        hi = 100000.0;
        break;
    case LG_SENSOR_VOLT:
        /* Rail sensors legitimately read 0 for unused rails such as CPU 1P8. */
        lo = -5.0;
        hi = 30.0;
        break;
    case LG_SENSOR_CURRENT:
        lo = -10.0;
        hi = 500.0;
        break;
    case LG_SENSOR_POWER:
        lo = -10.0;
        hi = 10000.0;
        break;
    default:
        break;
    }

    if (isnan(v)) {
        s->valid = false;
        copy_str(s->note, sizeof(s->note), "no reading");
        return false;
    }
    if (v < lo || v > hi) {
        s->valid = false;
        copy_str(s->note, sizeof(s->note), "out of range");
        return false;
    }

    if (sentinel_check) {
        if (s->has_min && v == s->min) {
            s->valid = false;
            copy_str(s->note, sizeof(s->note), "at declared minimum");
            return false;
        }
        if (s->has_max && v == s->max) {
            s->valid = false;
            copy_str(s->note, sizeof(s->note), "at declared maximum");
            return false;
        }
    }

    s->valid = true;
    s->note[0] = '\0';
    return true;
}

/* ----------------------------------------------------------- chip identity */

typedef struct {
    char dir[LG_PATH_MAX];  /* e.g. /sys/class/hwmon/hwmon12 */
    char name[LG_PATH_MAX]; /* e.g. /sys/class/hwmon/hwmon12/name */
    char nameval[LG_NAME_MAX];
    char driver[LG_NAME_MAX];
    char device[LG_PATH_MAX];
} lg_chip;

/*
 * Resolve the platform driver for a hwmon device. Two chips on this board
 * both write "nct6687" into their name file while their drivers are "nct6683"
 * and "nct6687", so the driver symlink is what actually identifies the chip.
 */
static void resolve_driver(lg_chip *chip)
{
    chip->driver[0] = '\0';
    chip->device[0] = '\0';

    char link[LG_PATH_MAX + 16];
    snprintf(link, sizeof(link), "%s/device", chip->dir);

    char resolved[LG_PATH_MAX];
    ssize_t n = readlink(link, resolved, sizeof(resolved) - 1);
    if (n > 0) {
        resolved[n] = '\0';
        copy_str(chip->device, sizeof(chip->device), resolved);
    }

    snprintf(link, sizeof(link), "%s/device/driver", chip->dir);
    n = readlink(link, resolved, sizeof(resolved) - 1);
    if (n > 0) {
        resolved[n] = '\0';
        const char *base = strrchr(resolved, '/');
        copy_str(chip->driver, sizeof(chip->driver), (base != NULL) ? base + 1 : resolved);
    }
}

/* Load the driver's advertised name, e.g. "nct6687" or "kraken2023". */
static void load_chip(lg_chip *chip, const char *dir)
{
    memset(chip, 0, sizeof(*chip));
    copy_str(chip->dir, sizeof(chip->dir), dir);
    snprintf(chip->name, sizeof(chip->name), "%s/name", dir);

    if (!read_text_file(chip->name, chip->nameval, sizeof(chip->nameval))) {
        const char *base = path_basename(dir);
        copy_str(chip->nameval, sizeof(chip->nameval), base);
    }
    trim(chip->nameval);
    resolve_driver(chip);
}

/* --------------------------------------------------------- sensor scanning */

/* Scale and units differ per class; keep raw in native units and scale at format. */
static double sensor_scale(lg_sensor_class cls, long raw)
{
    switch (cls) {
    case LG_SENSOR_TEMP:    return (double)raw / 1000.0;
    case LG_SENSOR_VOLT:    return (double)raw / 1000.0;
    case LG_SENSOR_CURRENT: return (double)raw / 1000.0;
    case LG_SENSOR_FAN:     return (double)raw;
    case LG_SENSOR_POWER:   return (double)raw / 1000000.0;
    default:                return (double)raw;
    }
}

static void add_sensor(lg_snapshot *snap, const lg_chip *chip, lg_sensor_class cls, int index,
                       const char *label, const char *input_path, long raw)
{
    if (snap->nsensors >= LG_MAX_SENSORS) {
        return;
    }

    lg_sensor *s = &snap->sensors[snap->nsensors];
    memset(s, 0, sizeof(*s));
    s->cls = cls;
    s->index = index;
    s->value = sensor_scale(cls, raw);

    copy_str(s->chip, sizeof(s->chip), chip->nameval);
    copy_str(s->driver, sizeof(s->driver), chip->driver);
    copy_str(s->sysfs, sizeof(s->sysfs), input_path);

    if (label != NULL && label[0] != '\0') {
        copy_str(s->label, sizeof(s->label), label);
    } else {
        const char *stem = (cls == LG_SENSOR_FAN) ? "fan" : (cls == LG_SENSOR_VOLT ||
                                                             cls == LG_SENSOR_CURRENT) ? "in" : "temp";
        snprintf(s->label, sizeof(s->label), "%s%d", stem, index);
    }

    /* Thresholds share the input's unit. The suffix buffers are larger than
     * the base so appending "_crit" can never truncate. */
    char base[LG_PATH_MAX];
    snprintf(base, sizeof(base), "%s", input_path);
    char *suffix = strstr(base, "_input");
    if (suffix != NULL) {
        *suffix = '\0';
    }

    long v = 0;
    char path[LG_PATH_SCRATCH];
    snprintf(path, sizeof(path), "%s_min", base);
    if (read_int_file(path, &v)) {
        s->min = sensor_scale(cls, v);
        s->has_min = true;
    }
    snprintf(path, sizeof(path), "%s_max", base);
    if (read_int_file(path, &v)) {
        s->max = sensor_scale(cls, v);
        s->has_max = true;
    }
    snprintf(path, sizeof(path), "%s_crit", base);
    if (read_int_file(path, &v)) {
        s->crit = sensor_scale(cls, v);
        s->has_crit = true;
    }

    reading_is_valid(s);
    snap->nsensors++;
}

/* Read a *_label file for a channel, if the driver publishes one. */
static void read_label(const lg_chip *chip, const char *dir, const char *stem, char *out, size_t cap)
{
    out[0] = '\0';
    char path[LG_PATH_SCRATCH];
    snprintf(path, sizeof(path), "%s/%s_label", dir, stem);
    if (!read_text_file(path, out, cap)) {
        out[0] = '\0';
        return;
    }
    trim(out);
    (void)chip;
}

/* Scan one hwmon device for temp, fan, volt and current channels. */
static void scan_sensors(lg_snapshot *snap, const lg_chip *chip, bool secondary)
{
    const size_t first_sensor = snap->nsensors;

    const char *dir = chip->dir;
    static const struct {
        const char *prefix;
        const char *stem;
        lg_sensor_class cls;
    } kinds[] = {
        {"temp", "temp", LG_SENSOR_TEMP},
        {"fan", "fan", LG_SENSOR_FAN},
        {"in", "in", LG_SENSOR_VOLT},
    };

    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        DIR *d = opendir(dir);
        if (d == NULL) {
            continue;
        }

        /* Two passes: current inputs first, then the *_curr alias some drivers
         * use to expose amperage on the same inN channel. */
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }

            int idx = match_channel(ent->d_name, kinds[k].prefix, "_input");
            if (idx < 0) {
                continue;
            }

            char path[LG_PATH_SCRATCH];
            /* Bound the node name explicitly: sysfs names are short, and an
             * unbounded copy here could overflow the scratch buffer. */
            snprintf(path, sizeof(path), "%.240s/%.32s", dir, ent->d_name);

            char label[LG_NAME_MAX];
            char stem[32];
            snprintf(stem, sizeof(stem), "%s%d", kinds[k].stem, idx);
            read_label(chip, dir, stem, label, sizeof(label));

            long raw = 0;
            if (read_int_file(path, &raw)) {
                add_sensor(snap, chip, kinds[k].cls, idx, label, path, raw);
            }
        }
        closedir(d);
    }

    /* Power via the driver-provided power/ subdirectory (energy counters). */
    char energy[LG_PATH_SCRATCH];
    snprintf(energy, sizeof(energy), "%s/power/energy1_input", dir);
    long raw = 0;
    if (read_int_file(energy, &raw)) {
        add_sensor(snap, chip, LG_SENSOR_POWER, 1, "Package energy", energy, raw);
    }

    for (size_t i = first_sensor; i < snap->nsensors; i++) {
        snap->sensors[i].secondary = secondary;
    }
}

/* --------------------------------------------------------------- controls */

static int pwm_raw_to_percent(long raw)
{
    if (raw < 0) {
        return -1;
    }
    long clamped = raw;
    if (clamped > 255) {
        clamped = 255;
    }
    return (int)((clamped * 100 + 127) / 255);
}

/*
 * Does this driver need pwm_enable=1 before a pwm write is honoured?
 * Verified empirically: nzxt_kraken3 accepts the write and discards it when
 * pwm_enable is 0. The nct6687 driver sets the manual bit itself, so it does
 * not need the extra write.
 */
static bool driver_needs_enable(const char *driver)
{
    return strcmp(driver, "nzxt_kraken3") == 0;
}

/*
 * Does entering manual mode reset the PWM register? Verified: setting
 * pwm_enable=1 on the Kraken drove pwm1 to 51 and pwm2 to 0, stalling the fan.
 */
static bool driver_enable_resets_pwm(const char *driver)
{
    return strcmp(driver, "nzxt_kraken3") == 0;
}

static void scan_controls(lg_snapshot *snap, const lg_chip *chip, const lg_snapshot *sensors)
{
    const char *dir = chip->dir;

    for (int i = 1; i <= 8; i++) {
        char pwm[LG_PATH_SCRATCH];
        snprintf(pwm, sizeof(pwm), "%s/pwm%d", dir, i);
        if (access(pwm, F_OK) != 0) {
            continue;
        }
        /* The read-only secondary Super-I/O is not a control surface. */
        if (!node_is_writable(pwm)) {
            continue;
        }
        if (snap->ncontrols >= LG_MAX_CONTROLS) {
            return;
        }

        lg_control *c = &snap->controls[snap->ncontrols];
        memset(c, 0, sizeof(*c));
        c->kind = LG_CTRL_HWMON;
        c->channel = i;
        c->duty = -1;
        c->duty_raw = -1;
        c->driver_min = -1;
        c->driver_max = -1;
        c->fan_rpm = -1;
        c->responds = -1;
        /* Replaced from the live reading; the initial value is set once. */
        c->enable_initial = -2;

        copy_str(c->chip, sizeof(c->chip), chip->nameval);
        copy_str(c->driver, sizeof(c->driver), chip->driver);
        copy_str(c->pwm_path, sizeof(c->pwm_path), pwm);

        /* Label from the paired fan header, falling back to the channel name. */
        char stem[32];
        snprintf(stem, sizeof(stem), "fan%d", i);
        char label[LG_NAME_MAX];
        read_label(chip, dir, stem, label, sizeof(label));
        if (label[0] != '\0') {
            snprintf(c->label, sizeof(c->label), "%s", label);
        } else {
            char fallback[LG_NAME_MAX];
            snprintf(fallback, sizeof(fallback), "%.60s pwm%d", chip->nameval, i);
            copy_str(c->label, sizeof(c->label), fallback);
        }

        /* Pair with the tachometer on the same channel. */
        char fan[LG_PATH_SCRATCH];
        snprintf(fan, sizeof(fan), "%s/%s_input", dir, stem);
        if (access(fan, F_OK) == 0) {
            long rpm = 0;
            if (read_int_file(fan, &rpm)) {
                copy_str(c->fan_path, sizeof(c->fan_path), fan);
                c->has_fan_pair = true;
                c->fan_rpm = (int)rpm;
            }
        }

        long v = 0;
        if (read_int_file(pwm, &v)) {
            c->duty_raw = (int)v;
            c->duty = pwm_raw_to_percent(v);
        }

        char enable[LG_PATH_SCRATCH];
        snprintf(enable, sizeof(enable), "%s/pwm%d_enable", dir, i);
        if (access(enable, F_OK) == 0) {
            copy_str(c->pwm_enable_path, sizeof(c->pwm_enable_path), enable);
            c->has_enable = true;
            if (read_int_file(enable, &v)) {
                c->enable_manual = (v == 1);
                c->enable_raw = (int)v;
            }
        }

        /* Driver-advertised limits, used to clamp and to detect dead channels. */
        snprintf(enable, sizeof(enable), "%s/pwm%d_min", dir, i);
        if (read_int_file(enable, &v)) {
            c->driver_min = (int)v;
        }
        snprintf(enable, sizeof(enable), "%s/pwm%d_max", dir, i);
        if (read_int_file(enable, &v)) {
            c->driver_max = (int)v;
        }

        c->needs_enable_first = driver_needs_enable(chip->driver);
        c->enable_resets_pwm = driver_enable_resets_pwm(chip->driver);
        c->is_aio = (strstr(chip->nameval, "kraken") != NULL);

        lg_control_make_key(c->key, sizeof(c->key), c->kind, c->driver, c->chip, c->channel);
        snap->ncontrols++;
    }

    (void)sensors;
}

/* ------------------------------------------------------------ source pick */

static bool label_contains(const char *hay, const char *needle)
{
    if (hay == NULL || needle == NULL) {
        return false;
    }
    size_t nl = strlen(needle);
    for (const char *p = hay; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nl && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) {
            return true;
        }
    }
    return false;
}

void lg_discover_pick_sources(lg_snapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    snap->cpu_temp_sensor = -1;
    snap->coolant_sensor = -1;
    snap->cpu_temp = NAN;
    snap->coolant_temp = NAN;

    int best = -1;
    for (size_t i = 0; i < snap->nsensors; i++) {
        const lg_sensor *s = &snap->sensors[i];
        if (s->cls != LG_SENSOR_TEMP || !s->valid) {
            continue;
        }

        /* Prefer the CPU package reading over individual cores. */
        if (label_contains(s->label, "package id 0")) {
            best = (int)i;
            break;
        }
        if (best < 0 && strcmp(s->chip, "coretemp") == 0) {
            best = (int)i;
        }
        if (best < 0) {
            best = (int)i;
        }
    }

    if (best >= 0) {
        snap->cpu_temp_sensor = best;
        snap->cpu_temp = snap->sensors[best].value;
    }

    /* Liquid temperature from the AIO's coolant channel, when present. */
    for (size_t i = 0; i < snap->nsensors; i++) {
        const lg_sensor *s = &snap->sensors[i];
        if (s->cls == LG_SENSOR_TEMP && s->valid && label_contains(s->label, "coolant")) {
            snap->coolant_sensor = (int)i;
            snap->coolant_temp = s->value;
            break;
        }
    }
}

/* ------------------------------------------------------------- entry point */

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

bool lg_discover(lg_snapshot *snap, const lg_discover_opts *opts)
{
    if (snap == NULL) {
        return false;
    }

    lg_discover_opts defaults;
    if (opts == NULL) {
        lg_discover_opts_init(&defaults);
        opts = &defaults;
    }
    const char *root = (opts->root != NULL) ? opts->root : LG_HWMON_ROOT;

    lg_snapshot_init(snap);

    /* Collect and sort hwmon directories so output order is deterministic. */
    char (*names)[LG_PATH_SCRATCH] = calloc(LG_MAX_CHIPS, sizeof(*names));
    size_t nnames = 0;

    DIR *d = opendir(root);
    if (d == NULL) {
        append_note(snap, "cannot open %s: %s", root, strerror(errno));
        free(names);
        return false;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && nnames < LG_MAX_CHIPS) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0) {
            continue;
        }
        snprintf(names[nnames], LG_PATH_SCRATCH, "%s/%s", root, ent->d_name);
        nnames++;
    }
    closedir(d);

    qsort(names, nnames, sizeof(*names), compare_names);

    /*
     * Work out which chip is the control surface for each hwmon name. On a
     * board with an nct6683 and an nct6687, both report the name "nct6687";
     * the one exposing writable pwm is authoritative and the other is marked
     * secondary so its mirror fan channels sort below the labelled ones.
     */
    char (*primary_driver)[LG_NAME_MAX] = calloc(nnames > 0 ? nnames : 1, sizeof(*primary_driver));
    if (primary_driver == NULL) {
        free(names);
        return false;
    }
    for (size_t i = 0; i < nnames; i++) {
        lg_chip chip;
        load_chip(&chip, names[i]);
        primary_driver[i][0] = '\0';

        char pwm_probe[LG_PATH_SCRATCH];
        snprintf(pwm_probe, sizeof(pwm_probe), "%s/pwm1", chip.dir);
        if (node_is_writable(pwm_probe)) {
            snprintf(primary_driver[i], sizeof(primary_driver[i]), "%s", chip.driver);
        }
    }

    for (size_t i = 0; i < nnames; i++) {
        lg_chip chip;
        load_chip(&chip, names[i]);

        bool has_collision = false;
        for (size_t j = 0; j < nnames; j++) {
            if (j == i) {
                continue;
            }
            lg_chip other;
            load_chip(&other, names[j]);
            if (strcmp(other.nameval, chip.nameval) == 0 &&
                primary_driver[j][0] != '\0' &&
                strcmp(primary_driver[j], chip.driver) != 0) {
                has_collision = true;
                break;
            }
        }

        scan_sensors(snap, &chip, has_collision);
        scan_controls(snap, &chip, NULL);
    }
    free(names);
    free(primary_driver);

    /*
     * liquidctl remains a fallback for AIOs with no hwmon exposure. Any device
     * already covered by a hwmon control is skipped so a single cooler is never
     * driven through two paths at once, which the legacy version could do.
     */
    bool have_aio_hwmon = false;
    for (size_t i = 0; i < snap->ncontrols; i++) {
        if (snap->controls[i].is_aio) {
            have_aio_hwmon = true;
            break;
        }
    }

    if (opts->use_liquidctl) {
        if (have_aio_hwmon) {
            append_note(snap, "AIO exposed via hwmon; liquidctl path suppressed to avoid dual control");
        } else {
            lg_liquidctl_discover(snap, opts->liquidctl_bin);
        }
    }

    lg_discover_pick_sources(snap);

    /*
     * Record the pwm_enable mode each channel had before we touched anything,
     * so exit can put it back rather than assuming a mode. A later refresh must
     * not overwrite the recorded value with one this application created.
     */
    for (size_t i = 0; i < snap->ncontrols; i++) {
        lg_control *c = &snap->controls[i];
        if (c->enable_initial == -2) {
            c->enable_initial = c->has_enable ? c->enable_raw : -1;
        }
    }

    if (snap->ncontrols == 0) {
        append_note(snap, "no writable PWM controls were found");
    }
    return true;
}

void lg_sensor_format(const lg_sensor *s, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    if (s == NULL) {
        snprintf(out, cap, "--");
        return;
    }
    if (!s->valid) {
        snprintf(out, cap, "-- (%s)", s->note[0] != '\0' ? s->note : "invalid");
        return;
    }

    switch (s->cls) {
    case LG_SENSOR_TEMP:
        snprintf(out, cap, "%.1f C", s->value);
        break;
    case LG_SENSOR_FAN:
        snprintf(out, cap, "%d rpm", (int)s->value);
        break;
    case LG_SENSOR_VOLT:
        if (s->value > 0.0 && s->value < 1.0) {
            snprintf(out, cap, "%.0f mV", s->value * 1000.0);
        } else {
            snprintf(out, cap, "%.2f V", s->value);
        }
        break;
    default:
        snprintf(out, cap, "%.3f %s", s->value, lg_sensor_class_unit(s->cls));
        break;
    }
}

void lg_control_format_duty(const lg_control *c, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    if (c == NULL || c->duty < 0) {
        snprintf(out, cap, "n/a");
        return;
    }
    snprintf(out, cap, "%d%%", c->duty);
}
