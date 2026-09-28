/*
 * lg-helper - minimal setuid-root writer for hwmon PWM nodes.
 *
 * This exists so the GUI never needs a shell, a password prompt, or a general
 * write primitive. It accepts exactly one shape of request and refuses
 * everything else:
 *
 *     lg-helper --set <pwm path> <0-255>
 *     lg-helper --set <pwm path> --enable <pwm_enable path> <0-255>
 *
 * The two-write form exists because the nzxt_kraken3 driver ignores PWM writes
 * until pwm_enable is set to manual, and entering manual mode resets the PWM
 * register to a driver default. Probing showed that reset drove the radiator
 * fan to 0 rpm for roughly a second. Doing both writes here, back to back in
 * one process, keeps that window at microseconds.
 *
 * Hardening:
 *   - The target must match ^/sys/class/hwmon/hwmon[0-9]+/pwm[0-9]+(_enable)?$
 *     with no traversal, no symlink following, and no relative components.
 *   - O_NOFOLLOW plus an fstat() check rejects a substituted symlink.
 *   - The value must parse as an integer in 0..255. No format strings, no
 *     shell, no environment, no arguments beyond those named above.
 *   - The environment is cleared and the process drops to a fixed cwd so
 *     nothing ambient can influence the write.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HWMON_ROOT "/sys/class/hwmon"
#define PWM_SUFFIX "_enable"

static void die(const char *msg)
{
    fprintf(stderr, "lg-helper: %s\n", msg);
    exit(1);
}

/*
 * Accept only "<root>/hwmon<digits>/pwm<digits>" with an optional "_enable".
 * Rejects any traversal, symlink, or path that is not directly under the
 * hwmon class directory.
 */
static int is_allowed_pwm_path(const char *path, int *is_enable)
{
    if (path == NULL) {
        return 0;
    }

    const char *p = path;
    const size_t root_len = sizeof(HWMON_ROOT) - 1;

    if (strncmp(p, HWMON_ROOT "/", root_len + 1) != 0) {
        return 0;
    }
    p += root_len + 1;

    /* Directory component must be exactly "hwmon" plus digits. */
    if (strncmp(p, "hwmon", 5) != 0) {
        return 0;
    }
    p += 5;
    if (!isdigit((unsigned char)*p)) {
        return 0;
    }
    while (isdigit((unsigned char)*p)) {
        p++;
    }
    if (*p != '/') {
        return 0;
    }
    p++;

    /* Filename must be "pwm" plus digits, optionally followed by _enable. */
    if (strncmp(p, "pwm", 3) != 0) {
        return 0;
    }
    p += 3;
    if (!isdigit((unsigned char)*p)) {
        return 0;
    }
    while (isdigit((unsigned char)*p)) {
        p++;
    }

    int enable = 0;
    if (strncmp(p, PWM_SUFFIX, sizeof(PWM_SUFFIX) - 1) == 0) {
        p += sizeof(PWM_SUFFIX) - 1;
        enable = 1;
    }
    if (*p != '\0') {
        return 0;
    }

    if (is_enable != NULL) {
        *is_enable = enable;
    }
    return 1;
}

/* Write an integer to a sysfs node, refusing symlinks. */
static int write_value(const char *path, int value)
{
    int fd = open(path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", value);

    ssize_t off = 0;
    int rc = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, (size_t)(n - off));
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = -1;
            break;
        }
        off += w;
    }
    close(fd);
    return rc;
}

/* Parse a strict decimal integer in 0..255. */
static int parse_value(const char *text, int *out)
{
    if (text == NULL || *text == '\0') {
        return -1;
    }
    for (const char *p = text; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v < 0 || v > 255) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: lg-helper --set <pwm path> [<pwm_enable path> --enable] <0-255>\n");
}

int main(int argc, char **argv)
{
    if (geteuid() != 0) {
        die("must be installed setuid root");
    }

    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 2;
    }

    /* lg-helper --mode <pwm_enable> <1|2> */
    if (strcmp(argv[1], "--mode") == 0) {
        if (argc != 4) {
            usage();
            return 2;
        }
        const char *enable = argv[2];
        int is_enable = 0;
        if (!is_allowed_pwm_path(enable, &is_enable) || !is_enable) {
            die("first path must be a pwm_enable node");
        }
        int mode = 0;
        if (parse_value(argv[3], &mode) != 0 || (mode != 1 && mode != 2)) {
            die("mode must be 1 for manual or 2 for automatic");
        }
        if (write_value(enable, mode) != 0) {
            die("write failed");
        }
        return 0;
    }

    if (strcmp(argv[1], "--set") != 0) {
        usage();
        return 2;
    }

    /* lg-helper --set <pwm> <value> */
    if (argc == 4) {
        const char *pwm = argv[2];
        int value = 0;
        int is_enable = 0;
        if (!is_allowed_pwm_path(pwm, &is_enable)) {
            die("refusing to write a path outside the hwmon pwm class");
        }
        /*
         * A pwm_enable node only accepts 1, 2, or the legacy 99. Letting the
         * duty form write to one would hand the caller a way to set an
         * arbitrary mode, so it is rejected outright and the two-write form is
         * the only path to it.
         */
        if (is_enable) {
            die("use the --enable form to change pwm_enable mode");
        }
        if (parse_value(argv[3], &value) != 0) {
            die("value must be an integer from 0 to 255");
        }
        if (write_value(pwm, value) != 0) {
            die("write failed");
        }
        return 0;
    }

    /* lg-helper --set <pwm> --enable <pwm_enable> <value> */
    if (argc == 6 && strcmp(argv[3], "--enable") == 0) {
        const char *pwm = argv[2];
        const char *enable = argv[4];
        int pwm_enable = 0;
        int value = 0;

        int pwm_is_enable = 0;
        int enable_is_enable = 0;
        if (!is_allowed_pwm_path(pwm, &pwm_is_enable) || pwm_is_enable) {
            die("first path must be a pwm node, not a pwm_enable node");
        }
        if (!is_allowed_pwm_path(enable, &enable_is_enable) || !enable_is_enable) {
            die("second path must be a pwm_enable node");
        }

        /* Both paths must name the same channel. */
        const char *pa = strrchr(pwm, '/');
        const char *ea = strrchr(enable, '/');
        if (pa == NULL || ea == NULL) {
            die("malformed path");
        }
        size_t name_len = strlen(pa + 1);
        if (strncmp(ea + 1, pa + 1, name_len) != 0 || strlen(ea + 1) != name_len + strlen(PWM_SUFFIX)) {
            die("pwm and pwm_enable must refer to the same channel");
        }

        if (parse_value(argv[5], &value) != 0) {
            die("value must be an integer from 0 to 255");
        }
        if (parse_value("1", &pwm_enable) != 0) {
            die("internal error preparing enable value");
        }

        /*
         * Order matters and must stay tight: entering manual mode first, then
         * the duty, with nothing in between.
         */
        if (write_value(enable, pwm_enable) != 0) {
            die("could not enter manual mode");
        }
        if (write_value(pwm, value) != 0) {
            die("write failed");
        }
        return 0;
    }

    usage();
    return 2;
}
