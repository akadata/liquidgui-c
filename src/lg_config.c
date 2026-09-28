/*
 * lg_config - per-control curve persistence and key migration.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lg_json.h"

#define LG_DIR_MODE 0700
#define LG_FILE_MODE 0600

void lg_config_init(lg_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->auto_apply = true;
    cfg->paused = false;
    cfg->exit_mode = LG_EXIT_HANDBACK;
    cfg->failsafe_temp = 90;
    cfg->stall_duty = 20;
    cfg->stall_samples = 3;
    snprintf(cfg->hwmon_root, sizeof(cfg->hwmon_root), "%s", "/sys/class/hwmon");
}

/* ------------------------------------------------------------------ paths */

static const char *xdg_config_home(void)
{
    const char *dir = getenv("XDG_CONFIG_HOME");
    if (dir != NULL && dir[0] == '/') {
        return dir;
    }
    return NULL;
}

const char *lg_config_path(void)
{
    static char buf[LG_PATH_SCRATCH];
    const char *home = xdg_config_home();
    if (home != NULL) {
        snprintf(buf, sizeof(buf), "%s/liquidgui/config.json", home);
    } else {
        const char *h = getenv("HOME");
        snprintf(buf, sizeof(buf), "%s/.config/liquidgui/config.json",
                 (h != NULL) ? h : ".");
    }
    return buf;
}

const char *lg_config_legacy_path(void)
{
    static char buf[LG_PATH_SCRATCH];
    const char *home = xdg_config_home();
    if (home != NULL) {
        snprintf(buf, sizeof(buf), "%s/liquidgui_curves.json", home);
    } else {
        const char *h = getenv("HOME");
        snprintf(buf, sizeof(buf), "%s/.config/liquidgui_curves.json", (h != NULL) ? h : ".");
    }
    return buf;
}

/* ----------------------------------------------------------------- file io */

static char *read_whole_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "re");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0 || len > (long)(16 * 1024 * 1024)) {
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
    if (len_out != NULL) {
        *len_out = got;
    }
    return buf;
}

/* mkdir -p for the directory containing path. */
static bool ensure_parent_dir(const char *path)
{
    char buf[LG_PATH_SCRATCH];
    snprintf(buf, sizeof(buf), "%.240s", path);

    char *slash = strrchr(buf, '/');
    if (slash == NULL) {
        return true;
    }
    *slash = '\0';
    if (buf[0] == '\0') {
        return true;
    }

    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(buf, LG_DIR_MODE) != 0 && errno != EEXIST) {
            return false;
        }
        *p = '/';
    }
    if (mkdir(buf, LG_DIR_MODE) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

/*
 * Atomic save: write to a sibling temporary, fsync, then rename. rename(2) is
 * atomic within a filesystem, so a reader never observes a partial file and a
 * crash cannot truncate the previous config.
 */
static bool write_atomic(const char *path, const char *data, size_t len)
{
    if (!ensure_parent_dir(path)) {
        return false;
    }

    char tmp[LG_PATH_SCRATCH];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, LG_FILE_MODE);
    if (fd < 0) {
        return false;
    }

    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(tmp);
            return false;
        }
        off += (size_t)n;
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return false;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }

    /* Persist the directory entry so the rename itself survives a crash. */
    char dirbuf[LG_PATH_SCRATCH];
    snprintf(dirbuf, sizeof(dirbuf), "%s", path);
    char *slash = strrchr(dirbuf, '/');
    if (slash != NULL) {
        *slash = '\0';
        int dfd = open(dirbuf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    return true;
}

/* ----------------------------------------------------------------- report */

static void report(lg_config *cfg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cfg->load_report, sizeof(cfg->load_report), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ curve entry */

lg_curve_entry *lg_config_find(lg_config *cfg, const char *key)
{
    if (cfg == NULL || key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cfg->nentries; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            return &cfg->entries[i];
        }
    }
    return NULL;
}

bool lg_config_remove(lg_config *cfg, const char *key)
{
    if (cfg == NULL || key == NULL) {
        return false;
    }
    for (size_t i = 0; i < cfg->nentries; i++) {
        if (strcmp(cfg->entries[i].key, key) != 0) {
            continue;
        }
        for (size_t j = i + 1; j < cfg->nentries; j++) {
            cfg->entries[j - 1] = cfg->entries[j];
        }
        cfg->nentries--;
        return true;
    }
    return false;
}

const lg_curve *lg_config_curve(lg_config *cfg, const lg_control *ctl)
{
    if (cfg == NULL || ctl == NULL) {
        return NULL;
    }

    lg_curve_entry *existing = lg_config_find(cfg, ctl->key);
    if (existing != NULL) {
        return &existing->curve;
    }

    if (cfg->nentries >= LG_MAX_CURVES) {
        return NULL;
    }

    lg_curve_entry *entry = &cfg->entries[cfg->nentries++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->key, sizeof(entry->key), "%s", ctl->key);
    lg_curve_init(&entry->curve);

    lg_point pts[LG_CURVE_MAX_POINTS];
    size_t n = lg_curve_default_points(pts, LG_CURVE_MAX_POINTS, ctl->label);
    lg_curve_set_points(&entry->curve, pts, n);

    return &entry->curve;
}

void lg_config_reset(lg_config *cfg, const lg_control *ctl)
{
    lg_curve_entry *entry = lg_config_find(cfg, (ctl != NULL) ? ctl->key : NULL);
    if (entry == NULL) {
        return;
    }

    lg_curve_init(&entry->curve);
    lg_point pts[LG_CURVE_MAX_POINTS];
    size_t n = lg_curve_default_points(pts, LG_CURVE_MAX_POINTS, ctl->label);
    lg_curve_set_points(&entry->curve, pts, n);
    snprintf(entry->key, sizeof(entry->key), "%s", ctl->key);
}

/* -------------------------------------------------------------- migration */

/*
 * Map a legacy path-based key onto a live control.
 *
 * Two strategies, in order of confidence:
 *
 *   1. Exact path match against the current snapshot. Correct as long as the
 *      hwmon numbering has not shifted since the config was written.
 *
 *   2. The legacy hwmon directory still exists and still exposes a *writable*
 *      pwm at that channel, and that node is one of the live controls. This
 *      recovers a curve across a renumbering without guessing.
 *
 * Strategy 2 deliberately refuses to fall back on channel number alone. Doing
 * so silently misapplies curves: a real configuration on this machine carries
 * entries for hwmon11 (a monitoring-only Super-I/O with read-only pwm) and for
 * hwmon4 (an SPD5118 DIMM hub that exposes no pwm whatsoever). Matching those
 * on channel alone would hand a DIMM hub's curve to the CPU fan. When neither
 * strategy applies the entry is reported as an orphan instead, which is
 * recoverable by the user; a wrong curve silently applied is not.
 */
static const lg_control *resolve_legacy(const char *legacy_key, const lg_snapshot *snap,
                                        const char *hwmon_root)
{
    if (snap == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < snap->ncontrols; i++) {
        if (snap->controls[i].pwm_path[0] != '\0' &&
            strcmp(legacy_key, snap->controls[i].pwm_path) == 0) {
            return &snap->controls[i];
        }
    }

    int channel = -1;
    if (!lg_key_parse_legacy(legacy_key, &channel)) {
        return NULL;
    }

    /* Recover the hwmon directory from the legacy key. */
    char number[16] = {0};
    const char *p = strstr(legacy_key, "hwmon/hwmon");
    if (p == NULL) {
        return NULL;
    }
    p += strlen("hwmon/hwmon");
    size_t n = 0;
    while (isdigit((unsigned char)*p) && n + 1 < sizeof(number)) {
        number[n++] = *p++;
    }
    if (n == 0 || *p != '/') {
        return NULL;
    }

    char probe[LG_PATH_SCRATCH];
    snprintf(probe, sizeof(probe), "%.200s/hwmon%s/pwm%d", hwmon_root, number, channel);

    struct stat st;
    if (stat(probe, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & S_IWUSR) == 0) {
        /* That node is gone or is not a control surface. */
        return NULL;
    }

    /* The node is a live control, so accept it. */
    for (size_t i = 0; i < snap->ncontrols; i++) {
        const lg_control *c = &snap->controls[i];
        if (c->kind == LG_CTRL_HWMON && c->channel == channel) {
            return c;
        }
    }
    return NULL;
}

/* Translate the old liquidctl pseudo-keys onto the hwmon AIO controls. */
static const lg_control *resolve_legacy_liquidctl(const char *legacy_key, const lg_snapshot *snap)
{
    int want_pump = (strcmp(legacy_key, "liquidctl:pump") == 0);
    if (!want_pump && strcmp(legacy_key, "liquidctl:fan") != 0) {
        return NULL;
    }

    for (size_t i = 0; i < snap->ncontrols; i++) {
        const lg_control *c = &snap->controls[i];
        if (!c->is_aio) {
            continue;
        }
        bool is_pump = (strstr(c->label, "Pump") != NULL) || (strstr(c->label, "pump") != NULL);
        if (is_pump == want_pump) {
            return c;
        }
    }
    return NULL;
}

static void load_points(lg_curve *curve, const lg_json *points)
{
    lg_point pts[LG_CURVE_MAX_POINTS];
    size_t n = 0;

    for (size_t i = 0; i < lg_json_len(points) && n < LG_CURVE_MAX_POINTS; i++) {
        const lg_json *pair = lg_json_at(points, i);
        pts[n].temp = (int)lg_json_as_num(lg_json_at(pair, 0), 0);
        pts[n].duty = (int)lg_json_as_num(lg_json_at(pair, 1), 0);
        n++;
    }
    lg_curve_set_points(curve, pts, n);
}

/*
 * The very first version of the file stored bare "fan"/"pump" point lists with
 * no per-control structure. Read those into the matching entries.
 */
static void load_legacy_flat(const lg_json *root, lg_config *cfg, const lg_snapshot *snap)
{
    static const struct {
        const char *field;
        const char *pseudo;
    } map[] = {
        {"pump", "liquidctl:pump"},
        {"fan", "liquidctl:fan"},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        const lg_json *points = lg_json_get(root, map[i].field);
        if (lg_json_type_of(points) != LG_JSON_ARR) {
            continue;
        }
        const lg_control *target = resolve_legacy_liquidctl(map[i].pseudo, snap);
        if (target == NULL) {
            cfg->orphans++;
            continue;
        }
        lg_curve_entry *entry = lg_config_find(cfg, target->key);
        if (entry == NULL) {
            if (cfg->nentries >= LG_MAX_CURVES) {
                continue;
            }
            entry = &cfg->entries[cfg->nentries++];
            memset(entry, 0, sizeof(*entry));
            snprintf(entry->key, sizeof(entry->key), "%s", target->key);
            lg_curve_init(&entry->curve);
            cfg->migrated++;
        }
        load_points(&entry->curve, points);
    }
}

bool lg_config_load(lg_config *cfg, const lg_snapshot *snap)
{
    if (cfg == NULL) {
        return false;
    }
    lg_config_init(cfg);

    const char *path = lg_config_path();
    char *text = read_whole_file(path, NULL);
    bool from_legacy_flat = false;

    if (text == NULL) {
        /* Fall back to the original file, if it is still around. */
        text = read_whole_file(lg_config_legacy_path(), NULL);
        from_legacy_flat = (text != NULL);
    }

    if (text == NULL) {
        report(cfg, "no configuration found; using defaults");
        return true;
    }

    const char *err = NULL;
    lg_json_doc *doc = lg_json_parse(text, &err);
    free(text);
    if (doc == NULL) {
        report(cfg, "configuration is not valid JSON (%s); using defaults",
               err != NULL ? err : "parse error");
        return false;
    }

    const lg_json *root = lg_json_root(doc);

    /* Version 0 stored bare "fan"/"pump" arrays at the top level. */
    if (lg_json_type_of(lg_json_get(root, "fan")) == LG_JSON_ARR ||
        lg_json_type_of(lg_json_get(root, "pump")) == LG_JSON_ARR) {
        load_legacy_flat(root, cfg, snap);
        from_legacy_flat = true;
    }

    cfg->auto_apply = lg_json_get_bool(root, "auto_apply", true);
    cfg->paused = lg_json_get_bool(root, "paused", false);
    cfg->default_source[0] = '\0';
    snprintf(cfg->default_source, sizeof(cfg->default_source), "%s",
             lg_json_get_str(root, "default_source", ""));

    const lg_json *settings = lg_json_get(root, "settings");
    if (settings != NULL) {
        cfg->exit_mode = (lg_exit_mode)lg_json_get_num(settings, "exit_mode", LG_EXIT_HANDBACK);
        cfg->failsafe_temp = (int)lg_json_get_num(settings, "failsafe_temp", cfg->failsafe_temp);
        cfg->stall_duty = (int)lg_json_get_num(settings, "stall_duty", cfg->stall_duty);
        cfg->stall_samples = (int)lg_json_get_num(settings, "stall_samples", cfg->stall_samples);
    }

    const lg_json *curves = lg_json_get(root, "curves");
    for (size_t i = 0; i < lg_json_len(curves); i++) {
        const char *key = lg_json_obj_key_at(curves, i);
        const lg_json *item = lg_json_obj_val_at(curves, i);
        if (key == NULL) {
            continue;
        }

        const lg_control *target = NULL;
        bool is_legacy = false;

        lg_curve_entry *direct = lg_config_find(cfg, key);
        if (direct == NULL) {
            /* Not a current key: try to migrate it. */
            if (strstr(key, "hwmon/hwmon") != NULL) {
                target = resolve_legacy(key, snap, cfg->hwmon_root);
                is_legacy = true;
            } else if (strncmp(key, "liquidctl:", 10) == 0) {
                target = resolve_legacy_liquidctl(key, snap);
                is_legacy = true;
            }
        } else {
            target = NULL;
        }

        char new_key[LG_KEY_MAX];
        if (direct != NULL) {
            snprintf(new_key, sizeof(new_key), "%s", direct->key);
        } else if (target != NULL) {
            snprintf(new_key, sizeof(new_key), "%s", target->key);
            cfg->migrated++;
        } else if (is_legacy) {
            cfg->orphans++;
            if (cfg->orphans_list[0] != '\0') {
                size_t used = strlen(cfg->orphans_list);
                snprintf(cfg->orphans_list + used, sizeof(cfg->orphans_list) - used, "%s,",
                         key);
            } else {
                snprintf(cfg->orphans_list, sizeof(cfg->orphans_list), "%s,", key);
            }
            continue;
        } else {
            /* A key we do not recognise and cannot map: keep it verbatim so a
             * temporarily absent device does not lose its curve. */
            snprintf(new_key, sizeof(new_key), "%s", key);
        }

        lg_curve_entry *entry = lg_config_find(cfg, new_key);
        if (entry == NULL) {
            if (cfg->nentries >= LG_MAX_CURVES) {
                continue;
            }
            entry = &cfg->entries[cfg->nentries++];
            memset(entry, 0, sizeof(*entry));
            snprintf(entry->key, sizeof(entry->key), "%s", new_key);
            lg_curve_init(&entry->curve);
        }

        load_points(&entry->curve, lg_json_get(item, "points"));
        entry->curve.enabled = lg_json_get_bool(item, "enabled", true);
        entry->curve.min_duty = (int)lg_json_get_num(item, "min_duty", 0);
        snprintf(entry->curve.source, sizeof(entry->curve.source), "%s",
                 lg_json_get_str(item, "source", ""));
    }

    /* The legacy file recorded the selection under its old key format. */
    const char *sel = lg_json_get_str(root, "selected_key", "");
    if (sel != NULL && sel[0] != '\0') {
        if (lg_config_find(cfg, sel) != NULL) {
            snprintf(cfg->selected_key, sizeof(cfg->selected_key), "%s", sel);
        } else {
            const lg_control *target = NULL;
            if (strstr(sel, "hwmon/hwmon") != NULL) {
                target = resolve_legacy(sel, snap, cfg->hwmon_root);
            } else if (strncmp(sel, "liquidctl:", 10) == 0) {
                target = resolve_legacy_liquidctl(sel, snap);
            }
            if (target != NULL) {
                snprintf(cfg->selected_key, sizeof(cfg->selected_key), "%s", target->key);
            }
        }
    }

    lg_json_doc_free(doc);

    if (from_legacy_flat || cfg->migrated > 0) {
        report(cfg, "migrated %zu curve(s) to stable keys; %zu could not be placed.",
               cfg->migrated, cfg->orphans);
    } else if (cfg->orphans > 0) {
        report(cfg, "%zu saved curve(s) refer to hardware that is not currently present.", cfg->orphans);
    } else {
        report(cfg, "loaded %zu curve(s).", cfg->nentries);
    }
    return true;
}

/* -------------------------------------------------------------- serialise */

char *lg_config_to_json(const lg_config *cfg)
{
    if (cfg == NULL) {
        return NULL;
    }

    lg_json_writer w;
    lg_json_writer_init(&w);

    lg_json_begin_object(&w);

    lg_json_key(&w, "version");
    lg_json_write_num(&w, LG_CONFIG_VERSION);
    lg_json_raw(&w, ",\n");

    lg_json_key(&w, "auto_apply");
    lg_json_write_bool(&w, cfg->auto_apply);
    lg_json_raw(&w, ",\n");

    lg_json_key(&w, "paused");
    lg_json_write_bool(&w, cfg->paused);
    lg_json_raw(&w, ",\n");

    lg_json_key(&w, "selected_key");
    lg_json_write_str(&w, cfg->selected_key);
    lg_json_raw(&w, ",\n");

    lg_json_key(&w, "default_source");
    lg_json_write_str(&w, cfg->default_source);
    lg_json_raw(&w, ",\n");

    lg_json_key(&w, "settings");
    lg_json_begin_object(&w);
    lg_json_key(&w, "exit_mode");
    lg_json_write_num(&w, (double)cfg->exit_mode);
    lg_json_raw(&w, ", ");
    lg_json_key(&w, "failsafe_temp");
    lg_json_write_num(&w, cfg->failsafe_temp);
    lg_json_raw(&w, ", ");
    lg_json_key(&w, "stall_duty");
    lg_json_write_num(&w, cfg->stall_duty);
    lg_json_raw(&w, ", ");
    lg_json_key(&w, "stall_samples");
    lg_json_write_num(&w, cfg->stall_samples);
    lg_json_end_object(&w);
    lg_json_raw(&w, ",\n");

    lg_json_key(&w, "curves");
    lg_json_begin_object(&w);
    for (size_t i = 0; i < cfg->nentries; i++) {
        const lg_curve_entry *e = &cfg->entries[i];
        if (i > 0) {
            lg_json_raw(&w, ",");
        }
        lg_json_raw(&w, "\n    ");
        lg_json_key(&w, e->key);
        lg_json_begin_object(&w);

        lg_json_key(&w, "points");
        lg_json_begin_array(&w);
        for (size_t p = 0; p < e->curve.npoints; p++) {
            if (p > 0) {
                lg_json_raw(&w, ", ");
            }
            lg_json_begin_array(&w);
            lg_json_write_num(&w, e->curve.points[p].temp);
            lg_json_raw(&w, ", ");
            lg_json_write_num(&w, e->curve.points[p].duty);
            lg_json_end_array(&w);
        }
        lg_json_end_array(&w);

        lg_json_raw(&w, ", ");
        lg_json_key(&w, "enabled");
        lg_json_write_bool(&w, e->curve.enabled);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "min_duty");
        lg_json_write_num(&w, e->curve.min_duty);
        if (e->curve.source[0] != '\0') {
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "source");
            lg_json_write_str(&w, e->curve.source);
        }
        lg_json_end_object(&w);
    }
    if (cfg->nentries > 0) {
        lg_json_raw(&w, "\n  ");
    }
    lg_json_end_object(&w);

    lg_json_end_object(&w);
    lg_json_raw(&w, "\n");

    return lg_json_writer_take(&w);
}

bool lg_config_save(const lg_config *cfg)
{
    char *text = lg_config_to_json(cfg);
    if (text == NULL) {
        return false;
    }
    bool ok = write_atomic(lg_config_path(), text, strlen(text));
    free(text);
    return ok;
}

/* ---------------------------------------------------------------- presets */

typedef struct {
    const char *name;
    const char *label;
    const lg_point points[6];
    size_t npoints;
} lg_preset;

/* Curves are the same shape the editor produces, so a preset is just a
 * replacement point list. Values chosen to be usable as a starting point on
 * typical board sensors. */
static const lg_preset g_presets[] = {
    {"silent",
     "Silent",
     {{30, 0}, {45, 20}, {55, 35}, {65, 60}, {75, 85}, {85, 100}},
     6},
    {"balanced",
     "Balanced",
     {{30, 30}, {40, 38}, {50, 50}, {60, 72}, {72, 100}},
     5},
    {"performance",
     "Performance",
     {{30, 40}, {40, 55}, {50, 75}, {60, 95}, {70, 100}},
     5},
    {"overclock",
     "Overclock",
     {{30, 55}, {40, 75}, {50, 95}, {60, 100}},
     4},
    {"max",
     "Maximum",
     {{30, 100}},
     1},
};

const char *const *lg_config_preset_names(void)
{
    static const char *names[sizeof(g_presets) / sizeof(g_presets[0]) + 1];
    for (size_t i = 0; i < sizeof(g_presets) / sizeof(g_presets[0]); i++) {
        names[i] = g_presets[i].name;
    }
    names[sizeof(g_presets) / sizeof(g_presets[0])] = NULL;
    return names;
}

bool lg_config_apply_preset(lg_config *cfg, const char *name)
{
    if (cfg == NULL || name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(g_presets) / sizeof(g_presets[0]); i++) {
        if (strcmp(g_presets[i].name, name) != 0) {
            continue;
        }
        for (size_t e = 0; e < cfg->nentries; e++) {
            lg_curve_set_points(&cfg->entries[e].curve, g_presets[i].points, g_presets[i].npoints);
            cfg->entries[e].curve.enabled = true;
        }
        return true;
    }
    return false;
}
