/*
 * lg_model - shared data types.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *lg_sensor_class_name(lg_sensor_class cls)
{
    switch (cls) {
    case LG_SENSOR_TEMP:    return "Temperature";
    case LG_SENSOR_FAN:     return "Fan speed";
    case LG_SENSOR_VOLT:    return "Voltage";
    case LG_SENSOR_CURRENT: return "Current";
    case LG_SENSOR_POWER:   return "Power";
    default:                return "Unknown";
    }
}

const char *lg_sensor_class_unit(lg_sensor_class cls)
{
    switch (cls) {
    case LG_SENSOR_TEMP:    return "C";
    case LG_SENSOR_FAN:     return "rpm";
    case LG_SENSOR_VOLT:    return "V";
    case LG_SENSOR_CURRENT: return "A";
    case LG_SENSOR_POWER:   return "W";
    default:                return "";
    }
}

double lg_py_round(double x)
{
    if (!isfinite(x)) {
        return x;
    }

    double floor_v = floor(x);
    double diff = x - floor_v;

    if (diff > 0.5) {
        return floor_v + 1.0;
    }
    if (diff < 0.5) {
        return floor_v;
    }

    /* Exactly .5: round towards the even neighbour. */
    return (fmod(floor_v, 2.0) == 0.0) ? floor_v : floor_v + 1.0;
}

void lg_snapshot_init(lg_snapshot *snap)
{
    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof(*snap));
    snap->source_sensor = -1;
    snap->cpu_temp_sensor = -1;
    snap->coolant_sensor = -1;
    snap->cpu_temp = NAN;
    snap->coolant_temp = NAN;
}

void lg_control_make_key(char *out, size_t cap, const lg_ctrl_kind kind, const char *driver,
                         const char *chip, int channel)
{
    if (out == NULL || cap == 0) {
        return;
    }

    /*
     * Prefer the platform driver name. On this machine both Super-I/O chips
     * report the hwmon name "nct6687" while their drivers are "nct6683" and
     * "nct6687", so the driver is the only reliable discriminator.
     */
    const char *ident = (driver != NULL && driver[0] != '\0') ? driver : chip;

    const char *prefix = (kind == LG_CTRL_LIQUIDCTL) ? "liquidctl" : "pwm";
    snprintf(out, cap, "%s:%s:%d", prefix, (ident != NULL) ? ident : "unknown", channel);
}

bool lg_key_parse_legacy(const char *key, int *channel_out)
{
    if (key == NULL) {
        return false;
    }

    /* Legacy hwmon keys embed the absolute sysfs path. */
    const char *p = strstr(key, "hwmon/hwmon");
    if (p == NULL) {
        return false;
    }

    p += strlen("hwmon/hwmon");
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }

    if (strncmp(p, "/pwm", 4) != 0) {
        return false;
    }
    p += 4;

    if (*p < '0' || *p > '9') {
        return false;
    }
    int channel = 0;
    while (*p >= '0' && *p <= '9') {
        channel = channel * 10 + (*p - '0');
        p++;
    }

    if (channel_out != NULL) {
        *channel_out = channel;
    }
    return true;
}
