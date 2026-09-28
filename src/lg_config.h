/*
 * lg_config - per-control curve persistence and key migration.
 *
 * The legacy file stored curves under keys that embed the absolute sysfs path,
 * e.g. "hwmon:/sys/class/hwmon/hwmon12/pwm3". Because hwmonN numbering is
 * assigned at probe order and shifts between boots, those keys were unstable:
 * a saved curve could be applied to a different header, or vanish. Loading
 * here resolves legacy keys against the live snapshot and rewrites the file
 * under stable keys, reporting anything it could not place.
 *
 * Writes are atomic: a temporary file in the same directory is written,
 * fsynced, then renamed over the target, so a crash or power loss cannot leave
 * a truncated config.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_CONFIG_H
#define LG_CONFIG_H

#include "lg_curve.h"
#include "lg_model.h"

#define LG_MAX_CURVES LG_MAX_CONTROLS

/* Schema version, bumped when the on-disk shape changes. */
#define LG_CONFIG_VERSION 2

typedef enum {
    LG_EXIT_RESTORE_SPEED = 0, /* write 100% to every control we touched */
    LG_EXIT_HANDBACK,          /* return channels to the EC's own curve */
    LG_EXIT_LEAVE,             /* leave duty as-is */
} lg_exit_mode;

typedef struct {
    char key[LG_KEY_MAX];
    lg_curve curve;
} lg_curve_entry;

typedef struct {
    lg_curve_entry entries[LG_MAX_CURVES];
    size_t nentries;

    char selected_key[LG_KEY_MAX];
    bool auto_apply;
    bool paused;

    /* Default source sensor for curves with no explicit source. */
    char default_source[128];

    lg_exit_mode exit_mode;
    int failsafe_temp;   /* force full speed above this, 0 disables */
    int stall_duty;      /* report a stall at or above this duty, 0 disables */
    int stall_samples;   /* consecutive samples before flagging a stall */

    /* hwmon class root, so legacy keys can be validated against the live tree. */
    char hwmon_root[LG_PATH_MAX];

    /* Diagnostics from the most recent load, surfaced in the interface. */
    char load_report[512];

    /* Orphaned legacy keys, reported rather than guessed at. */
    char orphans_list[512];
    size_t migrated;
    size_t orphans;
} lg_config;

void lg_config_init(lg_config *cfg);

/* Path of the user's configuration file, honouring $XDG_CONFIG_HOME. */
const char *lg_config_path(void);

/* Legacy path, kept for migration. */
const char *lg_config_legacy_path(void);

/*
 * Load the config, migrating legacy path-based keys onto the stable keys of
 * the supplied snapshot. Unresolvable entries are counted as orphans and
 * listed in cfg->load_report rather than being silently dropped.
 *
 * hwmon_root is the sysfs class root that legacy keys are validated against.
 * Pass the root discovery used, or NULL for /sys/class/hwmon. Tests pass a
 * fixture tree so migration does not depend on the host's hardware.
 */
bool lg_config_load(lg_config *cfg, const lg_snapshot *snap, const char *hwmon_root);

/* Atomically persist the config. Returns false on write failure. */
bool lg_config_save(const lg_config *cfg);

/*
 * Return the curve for a control, creating a default entry when absent. The
 * returned pointer stays valid until the next mutation of cfg.
 */
const lg_curve *lg_config_curve(lg_config *cfg, const lg_control *ctl);

/* Find an existing entry by key, or NULL. */
lg_curve_entry *lg_config_find(lg_config *cfg, const char *key);

/* Remove an entry. Returns true when something was removed. */
bool lg_config_remove(lg_config *cfg, const char *key);

/* Reset a control's curve to the built-in default for its label. */
void lg_config_reset(lg_config *cfg, const lg_control *ctl);

/* Serialise to JSON, for tests and preset export. Caller frees. */
char *lg_config_to_json(const lg_config *cfg);

/* Apply a named preset across every entry. Returns false for an unknown name. */
bool lg_config_apply_preset(lg_config *cfg, const char *name);

/* Names accepted by lg_config_apply_preset, NULL terminated. */
const char *const *lg_config_preset_names(void);

#endif /* LG_CONFIG_H */
