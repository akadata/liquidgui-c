/*
 * lg_model - shared data types for sensors, controls, and snapshots.
 *
 * Stable identity is the central concern here. The legacy Python keyed its
 * curves on the full sysfs path ("hwmon:/sys/class/hwmon/hwmon12/pwm3"), but
 * hwmonN numbering is assigned at probe order and changes between boots, so
 * saved curves could land on the wrong fan or vanish entirely. Keys here are
 * built from the driver identity plus the channel number, which are stable:
 *
 *     pwm:nct6687:3        motherboard header 3 on the NCT6687 Super-I/O
 *     pwm:kraken2023:1     pump on the Kraken
 *
 * Where two chips share a hwmon name (this board has both an nct6683 and an
 * nct6687, and both report "nct6687" in their name file), the platform driver
 * name is used to disambiguate and the key carries it.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_MODEL_H
#define LG_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#define LG_NAME_MAX 96
#define LG_PATH_MAX 256
/* Headroom for appending sysfs suffixes such as "_input" or "_enable". */
#define LG_PATH_SCRATCH (LG_PATH_MAX + 48)
#define LG_KEY_MAX 192
#define LG_NOTE_MAX 64

#define LG_MAX_CHIPS 32
#define LG_MAX_SENSORS 768
#define LG_MAX_CONTROLS 64

typedef enum {
    LG_SENSOR_TEMP = 0,
    LG_SENSOR_FAN,
    LG_SENSOR_VOLT,
    LG_SENSOR_CURRENT,
    LG_SENSOR_POWER,
    LG_SENSOR_CLASS_COUNT
} lg_sensor_class;

const char *lg_sensor_class_name(lg_sensor_class cls);
const char *lg_sensor_class_unit(lg_sensor_class cls);

typedef struct {
    lg_sensor_class cls;
    char chip[LG_NAME_MAX];   /* hwmon "name", e.g. nct6687 */
    char driver[LG_NAME_MAX]; /* platform driver, e.g. nct6687, disambiguator */
    char label[LG_NAME_MAX];  /* *_label contents, or a synthesised name */
    char sysfs[LG_PATH_MAX];  /* path to the _input node */
    int index;                /* channel number parsed from the filename */
    double value;             /* native units, not normalised */
    double min;
    double max;
    double crit;
    bool has_min;
    bool has_max;
    bool has_crit;
    /*
     * False when the reading is implausible for its class or matches a
     * driver-declared sentinel. Unconnected NCT6687 inputs report their
     * *_min floor, e.g. "PCIe x1" pinned at 193 C and "Virtual 0" at -63 C;
     * the legacy UI rendered those as if they were real temperatures.
     */
    bool valid;
    char note[LG_NOTE_MAX];

    /*
     * True when the owning chip is not the control surface for its hwmon name.
     * This board carries an nct6683 alongside an nct6687 and both write
     * "nct6687" into their name file; the nct6683 exposes a mirror set of
     * unlabelled fans alongside unique thermistor and VRM readings. The
     * interface sorts these below the labelled primary channels.
     */
    bool secondary;
} lg_sensor;

typedef enum {
    LG_CTRL_HWMON = 0,
    LG_CTRL_LIQUIDCTL = 1,
} lg_ctrl_kind;

typedef struct {
    lg_ctrl_kind kind;
    char key[LG_KEY_MAX];
    char chip[LG_NAME_MAX];
    char driver[LG_NAME_MAX];
    char label[LG_NAME_MAX];
    int channel; /* pwm index for hwmon, 0/1 for liquidctl */

    char pwm_path[LG_PATH_MAX];
    char pwm_enable_path[LG_PATH_MAX];
    char liquidctl_name[LG_NAME_MAX]; /* channel name for liquidctl */

    /*
     * True when the driver ignores pwm writes until pwm_enable has been set to
     * manual. Verified on nzxt_kraken3: with pwm_enable=0 a write to pwmN
     * returns success and changes nothing.
     */
    bool needs_enable_first;

    /*
     * True when entering manual mode resets the PWM register to a driver
     * default. Verified on nzxt_kraken3, where pwm_enable=1 drove pwm2 to 0 and
     * stalled the radiator fan for about a second. The helper writes
     * pwm_enable and pwm back to back to keep that window negligible.
     */
    bool enable_resets_pwm;

    /*
     * The raw pwm_enable value seen on the first discovery pass, before this
     * application touched anything. Restoring it on exit leaves the machine
     * exactly as it was found.
     *
     * The raw value is kept rather than the manual/auto boolean because the
     * three states are distinct: the nct6687 driver reports 1 for manual, 2 for
     * "controller runs its own curve", and 0 before it has been initialised.
     * Collapsing 0 into 2 would change the machine's state on exit.
     *
     * -1 when the node does not exist, -2 before the first pass records it.
     */
    int enable_initial;

    int duty;         /* percent, -1 when unknown */
    int duty_raw;     /* raw 0..255, -1 when unknown */
    int driver_min;   /* declared pwm floor, -1 when absent */
    int driver_max;   /* declared pwm ceiling, -1 when absent */
    bool has_enable;

    /*
     * Current pwm_enable state. Drivers in this family use 1 for manual and
     * 2 for "hand back to the EC's own curve"; legacy values of 99 are
     * normalised to 2 by the nct6687 driver.
     */
    bool enable_manual;
    int enable_raw; /* -1 when the node does not exist */

    /* Paired tachometer for RPM readback and stall detection. */
    char fan_path[LG_PATH_MAX];
    bool has_fan_pair;
    int fan_rpm;      /* -1 when unknown or unpaired */

    /*
     * Tri-state: -1 not yet tested, 0 confirmed non-responsive, 1 confirmed.
     * The nct6687 driver reports success even when the EC locked it out of its
     * own fan registers, so this can only be learned by reading the node back
     * after a write. A channel must not be reported as broken before a write
     * has actually been attempted.
     */
    int responds;

    /* Presentation group: AIO coolers above motherboard headers. */
    bool is_aio;
} lg_control;

/* Per-sensor and per-control state that changes over the life of a session. */
typedef struct {
    lg_sensor sensors[LG_MAX_SENSORS];
    size_t nsensors;

    lg_control controls[LG_MAX_CONTROLS];
    size_t ncontrols;

    /* Index of the sensor chosen to drive curves, or -1. */
    int source_sensor;

    /* Highest plausible temperature across valid sensors, and where it came
     * from. Used for the failsafe and the header readout. */
    double cpu_temp;
    int cpu_temp_sensor;

    /* Coolant / liquid temperature when a liquid loop is present, else NaN. */
    double coolant_temp;
    int coolant_sensor;

    char notes[LG_NOTE_MAX]; /* discovery warnings, surfaced in the UI */
} lg_snapshot;

void lg_snapshot_init(lg_snapshot *snap);

/*
 * Round half to even, matching Python's round(). C's round() rounds half away
 * from zero, which would disagree with the legacy implementation on exact .5
 * duty boundaries. Shared because curve evaluation and duty readback both need
 * the same rule.
 */
double lg_py_round(double x);

/*
 * Build the stable storage key for a control. Always writes a NUL-terminated
 * string; truncates rather than overflowing.
 */
void lg_control_make_key(char *out, size_t cap, const lg_ctrl_kind kind, const char *driver,
                         const char *chip, int channel);

/*
 * Parse a legacy key of the form "hwmon:/sys/class/hwmon/hwmon12/pwm3" and
 * report the channel number. Returns false when the key is not legacy-shaped.
 */
bool lg_key_parse_legacy(const char *key, int *channel_out);

#endif /* LG_MODEL_H */
