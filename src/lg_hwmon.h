/*
 * lg_hwmon - sensor and control discovery over /sys/class/hwmon.
 *
 * Fixes, relative to the legacy Python:
 *
 *   - Includes the kraken2023 AIO as first-class hwmon controls. The Python
 *     skipped that chip entirely and shelled out to `sudo liquidctl`, which
 *     forks a Python interpreter per write and needs root. The probe confirmed
 *     hwmon pwm writes drive the same pump and fan.
 *   - Distinguishes the two Super-I/O chips on this board by platform driver
 *     name instead of hwmon name, so the unlabelled nct6683 fan set no longer
 *     appears as a duplicate of the labelled nct6687 headers.
 *   - Reads voltages, currents, power, and thresholds, not just temps and fans.
 *   - Marks implausible readings invalid rather than displaying them.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_HWMON_H
#define LG_HWMON_H

#include "lg_model.h"

/* Root of the sysfs class tree. Overridable so tests can use a fixture tree. */
#ifndef LG_HWMON_ROOT
#define LG_HWMON_ROOT "/sys/class/hwmon"
#endif

typedef struct {
    const char *root;      /* defaults to LG_HWMON_ROOT */
    bool use_liquidctl;    /* probe liquidctl for HID-only AIOs */
    const char *liquidctl_bin;
} lg_discover_opts;

void lg_discover_opts_init(lg_discover_opts *opts);

/*
 * Populate snap from sysfs (and optionally liquidctl). Returns false only on
 * catastrophic failure; partial discovery is normal and reported via
 * snap->notes.
 */
bool lg_discover(lg_snapshot *snap, const lg_discover_opts *opts);

/* Resolve the preferred curve source sensor. Called by lg_discover. */
void lg_discover_pick_sources(lg_snapshot *snap);

/* Format a sensor reading for display, e.g. "46.5 C" or "--". */
void lg_sensor_format(const lg_sensor *sensor, char *out, size_t cap);

/* Format a control's current duty as a percentage string. */
void lg_control_format_duty(const lg_control *ctl, char *out, size_t cap);

#endif /* LG_HWMON_H */
