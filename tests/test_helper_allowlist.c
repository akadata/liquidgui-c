/*
 * Allowlist tests for the setuid helper's path validation.
 *
 * The helper is the only component that runs as root, so its input handling is
 * tested directly against the attack cases rather than trusted. These run
 * against a build of the helper compiled WITHOUT setuid, exercising the
 * validator through a test entry point.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include <stdio.h>
#include <string.h>

/* Pull in the helper implementation so the validator is tested as shipped. */
int helper_main(int argc, char **argv);
#define main helper_main
#include "../helper/lg-helper.c"
#undef main

static int failures = 0;
static long checks = 0;

static void expect_allow(const char *path, int want_enable)
{
    checks++;
    int is_enable = -1;
    if (!is_allowed_pwm_path(path, &is_enable)) {
        printf("FAIL: rejected valid path %s\n", path);
        failures++;
        return;
    }
    if (is_enable != want_enable) {
        printf("FAIL: %s enable flag = %d, expected %d\n", path, is_enable, want_enable);
        failures++;
    }
}

static void expect_deny(const char *path, const char *why)
{
    checks++;
    if (is_allowed_pwm_path(path, NULL)) {
        printf("FAIL: allowed unsafe path %s (%s)\n", path, why);
        failures++;
    }
}

int main(void)
{
    /* --- must be accepted --- */
    expect_allow("/sys/class/hwmon/hwmon12/pwm1", 0);
    expect_allow("/sys/class/hwmon/hwmon12/pwm8", 0);
    expect_allow("/sys/class/hwmon/hwmon0/pwm0", 0);
    expect_allow("/sys/class/hwmon/hwmon12/pwm1_enable", 1);
    expect_allow("/sys/class/hwmon/hwmon12345/pwm99_enable", 1);

    /* --- must be rejected --- */
    expect_deny("/etc/passwd", "arbitrary file");
    expect_deny("/etc/shadow", "arbitrary file");
    expect_deny("/bin/sh", "shell binary");
    expect_deny("/sys/class/hwmon/hwmon12/name", "not a pwm node");
    expect_deny("/sys/class/hwmon/hwmon12/temp1_input", "not a pwm node");
    expect_deny("/sys/class/hwmon/hwmon12/fan1_input", "not a pwm node");
    expect_deny("/sys/class/hwmon/hwmon12/pwm1_input", "read-only node");
    expect_deny("/sys/class/hwmon/hwmon12/device/pwm1", "nested below a symlink");
    expect_deny("/sys/class/hwmon/../etc/passwd", "traversal");
    expect_deny("/sys/class/hwmon/hwmon12/../../../etc/passwd", "traversal");
    expect_deny("/sys/class/hwmon/hwmon12/pwm1 ", "trailing space");
    expect_deny("/sys/class/hwmon/hwmon12/pwm1x", "trailing junk");
    expect_deny("/sys/class/hwmon/hwmon12/pwmx1", "no digits");
    expect_deny("/sys/class/hwmon/hwmon12/pwm", "no digits");
    expect_deny("/sys/class/hwmon/hwmon/pwm1", "no hwmon digits");
    expect_deny("/sys/class/hwmon/pwm1", "no hwmon component");
    expect_deny("hwmon12/pwm1", "relative path");
    expect_deny("./hwmon12/pwm1", "relative path");
    expect_deny("/sys/class/hwmonx/hwmon12/pwm1", "wrong root");
    expect_deny("/sys/class/hwmon/hwmon12/pwm1_enable_extra", "suffix plus junk");
    expect_deny("", "empty");
    expect_deny(NULL, "null");

    /* --- value parsing --- */
    struct {
        const char *text;
        int ok;
    } values[] = {
        {"0", 1}, {"1", 1}, {"128", 1}, {"255", 1},
        {"256", 0}, {"-1", 0}, {"1000", 0}, {"", 0},
        {"12x", 0}, {"x12", 0}, {" 12", 0}, {"12 ", 0},
        {"1.5", 0}, {"+12", 0}, {"0x10", 0}, {"1e2", 0},
    };

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        checks++;
        int v = -1;
        int rc = parse_value(values[i].text, &v);
        if ((rc == 0) != (values[i].ok != 0)) {
            printf("FAIL: value \"%s\" accepted=%d expected_ok=%d\n",
                   values[i].text, rc == 0, values[i].ok);
            failures++;
        }
    }

    printf("%ld allowlist checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
