/*
 * Configuration, key stability and migration tests.
 *
 * The migration cases are drawn from a real configuration that the previous
 * implementation produced, which contained entries keyed on hwmon11, hwmon12
 * and hwmon4. hwmon4 is an SPD5118 DIMM hub that exposes no PWM at all, and
 * the nct6687 controls have since been renumbered, so those keys no longer
 * resolve by path and have to be recovered by driver identity.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/lg_config.h"
#include "../src/lg_curve.h"
#include "../src/lg_model.h"

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

/* ---------------------------------------------------------------- fixture */

static void add_control(lg_snapshot *s, const char *driver, const char *chip, const char *label,
                        int channel, const char *pwm_path, bool aio)
{
    lg_control *c = &s->controls[s->ncontrols++];
    memset(c, 0, sizeof(*c));
    snprintf(c->driver, sizeof(c->driver), "%s", driver);
    snprintf(c->chip, sizeof(c->chip), "%s", chip);
    snprintf(c->label, sizeof(c->label), "%s", label);
    c->channel = channel;
    snprintf(c->pwm_path, sizeof(c->pwm_path), "%s", pwm_path);
    c->kind = LG_CTRL_HWMON;
    c->is_aio = aio;
    c->duty = -1;
    c->driver_min = -1;
    c->driver_max = -1;
    c->fan_rpm = -1;
    lg_control_make_key(c->key, sizeof(c->key), c->kind, c->driver, c->chip, c->channel);
}

static void build_fixture(lg_snapshot *s)
{
    lg_snapshot_init(s);

    /* Current reality: the controls live on hwmon12, driver nct6687. */
    static const char *labels[8] = {"CPU Fan",      "Pump Fan",     "System Fan #1", "System Fan #2",
                                    "System Fan #3", "System Fan #4", "System Fan #5", "System Fan #6"};
    for (int i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon12/pwm%d", i + 1);
        add_control(s, "nct6687", "nct6687", labels[i], i + 1, path, false);
    }

    char apath[64];
    snprintf(apath, sizeof(apath), "/sys/class/hwmon/hwmon5/pwm1");
    add_control(s, "nzxt_kraken3", "kraken2023", "Pump speed", 1, apath, true);
    snprintf(apath, sizeof(apath), "/sys/class/hwmon/hwmon5/pwm2");
    add_control(s, "nzxt_kraken3", "kraken2023", "Fan speed", 2, apath, true);
}

/* Write a file into the temporary config home. */
/* --------------------------------------------------------------- fixture fs */

/*
 * Migration validates legacy keys against a real directory tree, so the test
 * builds one instead of relying on whatever hardware the runner happens to
 * have. It mirrors the situation this test exists for: a writable control chip,
 * a second chip with read-only pwm, and a hub with no pwm at all.
 */
static char g_fixture[256];

static void fixture_write(const char *rel, int mode)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_fixture, rel);
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs("128\n", f);
        fclose(f);
    }
    chmod(path, (mode_t)mode);
}

static void build_fixture_tree(void)
{
    snprintf(g_fixture, sizeof(g_fixture), "/tmp/liquidgui-fixture-%ld", (long)getpid());
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_fixture);
    if (system(cmd) != 0) {
        /* non-fatal */
    }

    /* The control chip: writable pwm, which is what makes hwmon12 resolvable. */
    for (int i = 1; i <= 8; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "hwmon12/pwm%d", i);
        char path[512];
        snprintf(path, sizeof(path), "%s/hwmon12", g_fixture);
        mkdir(path, 0755);
        fixture_write(rel, 0644);
    }

    /* The secondary chip: pwm present but read-only, so it must not resolve. */
    for (int i = 1; i <= 8; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "hwmon11/pwm%d", i);
        char path[512];
        snprintf(path, sizeof(path), "%s/hwmon11", g_fixture);
        mkdir(path, 0755);
        fixture_write(rel, 0444);
    }

    /* A hub with no pwm whatsoever: hwmon4 gets a temperature node only. */
    char path[512];
    snprintf(path, sizeof(path), "%s/hwmon4", g_fixture);
    mkdir(path, 0755);
    fixture_write("hwmon4/temp1_input", 0444);
}

static void remove_fixture_tree(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_fixture);
    if (system(cmd) != 0) {
        /* non-fatal */
    }
}

/* Create the parent directory of path, so the loader can find our fixture. */
static void ensure_parent(const char *path)
{
    char buf[LG_PATH_SCRATCH];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", buf);
    if (system(cmd) != 0) {
        /* Non-fatal: the load below will report the real problem. */
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "we");
    if (f == NULL) {
        printf("FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    fputs(text, f);
    fclose(f);
}

/* ------------------------------------------------------------------ tests */

static void test_keys(void)
{
    char key[LG_KEY_MAX];

    /* The key must not contain the hwmon number, which is what broke before. */
    lg_control_make_key(key, sizeof(key), LG_CTRL_HWMON, "nct6687", "nct6687", 3);
    check_str(key, "pwm:nct6687:3", "hwmon control key is stable");
    check(strstr(key, "hwmon") == NULL, "control key omits the hwmon index");

    /* Two chips sharing a hwmon name must not collide. */
    char other[LG_KEY_MAX];
    lg_control_make_key(other, sizeof(other), LG_CTRL_HWMON, "nct6683", "nct6687", 1);
    check_str(other, "pwm:nct6683:1", "second chip gets a distinct key");
    check(strcmp(key, other) != 0, "nct6683 and nct6687 keys differ");

    lg_control_make_key(key, sizeof(key), LG_CTRL_LIQUIDCTL, NULL, "NZXT Kraken 2023", 0);
    check_str(key, "liquidctl:NZXT Kraken 2023:0", "liquidctl key format");
}

static void test_legacy_key_parsing(void)
{
    int ch = -1;
    check(lg_key_parse_legacy("hwmon:/sys/class/hwmon/hwmon12/pwm3", &ch) && ch == 3,
          "parses a legacy path key");
    check(lg_key_parse_legacy("hwmon:/sys/class/hwmon/hwmon5/pwm1_enable", &ch) && ch == 1,
          "parses a legacy pwm_enable key");
    check(!lg_key_parse_legacy("liquidctl:fan", &ch), "rejects a non-path key");
    check(!lg_key_parse_legacy("pwm:nct6687:3", &ch), "rejects a modern key");
}

static void test_migration(const char *config_path)
{
    /*
     * Reproduces the real config: eight controls keyed on hwmon12, eight stale
     * entries on hwmon11 (a renumbering), and eight on hwmon4 which is an SPD
     * DIMM hub with no PWM whatsoever.
     */
    char text[8192];
    size_t off = 0;
    off += (size_t)snprintf(text + off, sizeof(text) - off,
                            "{\n  \"selected_key\": \"hwmon:/sys/class/hwmon/hwmon12/pwm1\",\n"
                            "  \"auto_apply\": true,\n  \"curves\": {\n");

    for (int i = 1; i <= 8; i++) {
        off += (size_t)snprintf(text + off, sizeof(text) - off,
                                "    \"hwmon:/sys/class/hwmon/hwmon12/pwm%d\": "
                                "{\"points\": [[30, 20], [60, 80]], \"enabled\": true},\n",
                                i);
    }
    /* Same physical chip, old numbering: must be recovered by channel. */
    for (int i = 1; i <= 8; i++) {
        off += (size_t)snprintf(text + off, sizeof(text) - off,
                                "    \"hwmon:/sys/class/hwmon/hwmon11/pwm%d\": "
                                "{\"points\": [[35, 33]], \"enabled\": true},\n",
                                i);
    }
    /* Not a control surface at all: must be reported, not silently applied. */
    for (int i = 1; i <= 3; i++) {
        off += (size_t)snprintf(text + off, sizeof(text) - off,
                                "    \"hwmon:/sys/class/hwmon/hwmon4/pwm%d\": "
                                "{\"points\": [[30, 10]], \"enabled\": true},\n",
                                i);
    }

    off += (size_t)snprintf(text + off, sizeof(text) - off,
                            "    \"liquidctl:pump\": {\"points\": [[30, 100]], \"enabled\": true},\n"
                            "    \"liquidctl:fan\": {\"points\": [[30, 90]], \"enabled\": true}\n"
                            "  }\n}\n");
    write_file(config_path, text);

    lg_snapshot snap;
    build_fixture(&snap);

    lg_config cfg;
    check(lg_config_load(&cfg, &snap, g_fixture), "legacy config loads");

    check(cfg.migrated > 0, "at least one curve was migrated");
    check(cfg.orphans > 0, "unresolvable entries are counted as orphans");
    printf("  migrated=%zu orphans=%zu entries=%zu\n", cfg.migrated, cfg.orphans, cfg.nentries);

    /* The old selection must resolve to the new stable key. */
    check_str(cfg.selected_key, "pwm:nct6687:1", "selected key migrated");

    /* Every hwmon12 curve must exist under its stable key. */
    for (int i = 1; i <= 8; i++) {
        char want[LG_KEY_MAX];
        snprintf(want, sizeof(want), "pwm:nct6687:%d", i);
        check(lg_config_find(&cfg, want) != NULL, "header curve present under a stable key");
    }

    /* The Kraken curves must have moved off the liquidctl pseudo-keys. */
    check(lg_config_find(&cfg, "pwm:nzxt_kraken3:1") != NULL,
          "legacy liquidctl:pump curve mapped onto the Kraken pump");
    check(lg_config_find(&cfg, "pwm:nzxt_kraken3:2") != NULL,
          "legacy liquidctl:fan curve mapped onto the Kraken fan");

    /* Nothing may reference a raw sysfs path any more. */
    for (size_t i = 0; i < cfg.nentries; i++) {
        check(strstr(cfg.entries[i].key, "/sys/") == NULL,
              "no stored key contains an absolute sysfs path");
    }

    /* The SPD hub entries must not have been applied to real controls. */
    for (size_t i = 0; i < cfg.nentries; i++) {
        const lg_curve *cv = &cfg.entries[i].curve;
        if (cv->npoints == 1 && cv->points[0].duty == 10) {
            printf("FAIL: an SPD hub curve leaked onto %s\n", cfg.entries[i].key);
            failures++;
        }
    }
}

static void test_roundtrip(const char *config_path)
{
    lg_snapshot snap;
    build_fixture(&snap);

    lg_config cfg;
    lg_config_init(&cfg);
    for (size_t i = 0; i < snap.ncontrols; i++) {
        const lg_curve *cv = lg_config_curve(&cfg, &snap.controls[i]);
        check(cv != NULL, "default curve is created on demand");
    }
    cfg.auto_apply = false;
    cfg.failsafe_temp = 85;
    check(lg_config_save(&cfg), "config saves");

    lg_config reloaded;
    check(lg_config_load(&reloaded, &snap, g_fixture), "config reloads");
    check(reloaded.auto_apply == false, "auto_apply round-trips");
    check(reloaded.failsafe_temp == 85, "failsafe threshold round-trips");
    check(reloaded.nentries == cfg.nentries, "entry count round-trips");

    for (size_t i = 0; i < cfg.nentries; i++) {
        const lg_curve *a = &cfg.entries[i].curve;
        const lg_curve_entry *b = lg_config_find(&reloaded, cfg.entries[i].key);
        if (b == NULL) {
            checks++;
            printf("FAIL: %s did not survive the round trip\n", cfg.entries[i].key);
            failures++;
            continue;
        }
        check(b->curve.npoints == a->npoints, "point count round-trips");
        for (size_t p = 0; p < a->npoints && p < b->curve.npoints; p++) {
            check(b->curve.points[p].temp == a->points[p].temp &&
                      b->curve.points[p].duty == a->points[p].duty,
                  "point values round-trip");
        }
    }
    (void)config_path;
}

static void test_presets(void)
{
    lg_config cfg;
    lg_config_init(&cfg);
    cfg.nentries = 2;
    memset(&cfg.entries[0], 0, sizeof(cfg.entries[0]));
    memset(&cfg.entries[1], 0, sizeof(cfg.entries[1]));
    snprintf(cfg.entries[0].key, sizeof(cfg.entries[0].key), "pwm:nct6687:1");
    snprintf(cfg.entries[1].key, sizeof(cfg.entries[1].key), "pwm:nct6687:2");
    lg_curve_init(&cfg.entries[0].curve);
    lg_curve_init(&cfg.entries[1].curve);

    check(lg_config_apply_preset(&cfg, "max"), "max preset applies");
    for (size_t i = 0; i < cfg.nentries; i++) {
        check(cfg.entries[i].curve.npoints == 1 && cfg.entries[i].curve.points[0].duty == 100,
              "max preset is full speed everywhere");
        check(lg_curve_duty(&cfg.entries[i].curve, 20.0) == 100,
              "max preset evaluates to 100 percent");
    }

    check(lg_config_apply_preset(&cfg, "silent"), "silent preset applies");
    check(lg_curve_duty(&cfg.entries[0].curve, 30.0) == 0, "silent preset is off when cool");
    check(lg_curve_duty(&cfg.entries[0].curve, 90.0) == 100, "silent preset is full when hot");

    check(!lg_config_apply_preset(&cfg, "nonexistent"), "unknown preset is rejected");
}

static void test_curve_normalisation(void)
{
    lg_point in[4] = {{30, 50}, {20, 150}, {-40, -10}, {200, 60}};
    lg_point out[4];
    size_t n = lg_curve_normalize(in, 4, out, 4);

    check(n == 4, "all points retained");
    check(out[0].temp == 30, "first temperature kept");
    /* The second point is below the first, so it is pulled up to match. */
    check(out[1].temp == 30, "decreasing temperature is clamped forward");
    check(out[1].duty == 100, "duty clamped to 100");
    check(out[2].duty == 0, "duty clamped to 0");
    check(out[3].temp == 110, "temperature clamped to the hard maximum");
}

int main(int argc, char **argv)
{
    /* Honour an explicit path, otherwise exercise the real lookup. */
    const char *config_path = (argc > 1) ? argv[1] : lg_config_path();
    ensure_parent(config_path);

    build_fixture_tree();

    test_keys();
    test_legacy_key_parsing();
    test_migration(config_path);
    unlink(config_path);
    test_roundtrip(config_path);
    test_presets();
    test_curve_normalisation();
    unlink(config_path);

    remove_fixture_tree();

    printf("%ld checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
