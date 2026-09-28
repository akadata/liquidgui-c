/*
 * lg_control - applying duty to a control.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lg_liquidctl.h"

/*
 * Install locations tried in order. The /usr/local entries cover installs made
 * before the prefix moved to /usr, so an existing setuid helper keeps working
 * without being reinstalled.
 */
static const char *const g_helper_candidates[] = {
    LG_HELPER_PATH,
    "/usr/local/libexec/liquidgui/lg-helper",
    "/usr/local/bin/lg-helper",
};

int lg_control_duty_to_raw(const lg_control *ctl, int duty_percent)
{
    if (duty_percent < 0) {
        return 0;
    }
    if (duty_percent > 100) {
        duty_percent = 100;
    }

    int raw = (duty_percent * 255 + 50) / 100;

    if (ctl != NULL) {
        if (ctl->driver_min >= 0 && raw < ctl->driver_min) {
            raw = ctl->driver_min;
        }
        if (ctl->driver_max >= 0 && raw > ctl->driver_max) {
            raw = ctl->driver_max;
        }
    }
    if (raw < 0) {
        raw = 0;
    }
    if (raw > 255) {
        raw = 255;
    }
    return raw;
}

/* --------------------------------------------------------------- privilege */

void lg_priv_init(lg_priv *priv)
{
    if (priv == NULL) {
        return;
    }
    memset(priv, 0, sizeof(*priv));
    priv->mode = LG_PRIV_NONE;
    snprintf(priv->helper_path, sizeof(priv->helper_path), "%s", LG_HELPER_PATH);
}

/* True when the file is a regular file with the setuid bit set. */
static bool is_setuid_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        return false;
    }
    if ((st.st_mode & S_ISUID) == 0) {
        return false;
    }
    return access(path, X_OK) == 0;
}

void lg_priv_detect(lg_priv *priv)
{
    if (priv == NULL) {
        return;
    }
    lg_priv_init(priv);

    if (geteuid() == 0) {
        priv->mode = LG_PRIV_DIRECT;
        priv->available = true;
        snprintf(priv->detail, sizeof(priv->detail), "running as root");
        return;
    }

    for (size_t i = 0; i < sizeof(g_helper_candidates) / sizeof(g_helper_candidates[0]); i++) {
        if (!is_setuid_executable(g_helper_candidates[i])) {
            continue;
        }
        snprintf(priv->helper_path, sizeof(priv->helper_path), "%s", g_helper_candidates[i]);
        priv->mode = LG_PRIV_HELPER;
        priv->available = true;
        snprintf(priv->detail, sizeof(priv->detail), "privileged helper");
        return;
    }

    /* No helper installed: fall back to a one-shot polkit or sudo prompt. */
    const char *pkexec = "/usr/bin/pkexec";
    if (access(pkexec, X_OK) == 0) {
        priv->mode = LG_PRIV_PKEXEC;
        priv->available = true;
        snprintf(priv->detail, sizeof(priv->detail),
                 "no setuid helper found; using polkit (expect an authentication prompt)");
        return;
    }
    if (access("/usr/bin/sudo", X_OK) == 0) {
        priv->mode = LG_PRIV_SUDO;
        priv->available = true;
        snprintf(priv->detail, sizeof(priv->detail),
                 "no setuid helper found; using sudo");
        return;
    }

    snprintf(priv->detail, sizeof(priv->detail),
             "no privileged write path available; run 'make install' as root to install the helper");
}

const char *lg_priv_describe(const lg_priv *priv)
{
    if (priv == NULL) {
        return "unknown";
    }
    return priv->detail;
}

/* ------------------------------------------------------------------ writes */

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (err == NULL || cap == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* Write directly, used when already root. */
static bool write_sysfs(const char *path, int value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d\n", value);
    ssize_t off = 0;
    bool ok = true;

    while (off < n) {
        ssize_t w = write(fd, buf + off, (size_t)n - (size_t)off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        off += w;
    }
    close(fd);
    return ok;
}

/* Run an external command, discarding output. Returns its exit status. */
static int run_quiet(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/*
 * Apply a duty through the selected privilege mechanism.
 *
 * For the Kraken the enable write and the pwm write are passed to the helper
 * as a single request so the driver never sits in manual mode with a reset
 * register value. Both are still separate sysfs writes, but they happen
 * microseconds apart inside one process instead of across two sudo forks.
 */
static bool apply_hwmon(const lg_priv *priv, const lg_control *ctl, int raw, char *err, size_t cap)
{
    const bool need_enable = ctl->needs_enable_first && ctl->has_enable &&
                             ctl->pwm_enable_path[0] != '\0';
    /* Only re-assert manual mode when we are not already in it. */
    const bool assert_enable = need_enable && !ctl->enable_manual;

    switch (priv->mode) {
    case LG_PRIV_DIRECT: {
        if (assert_enable && !write_sysfs(ctl->pwm_enable_path, 1)) {
            set_err(err, cap, "cannot enter manual mode on %s: %s", ctl->pwm_enable_path,
                    strerror(errno));
            return false;
        }
        if (!write_sysfs(ctl->pwm_path, raw)) {
            set_err(err, cap, "cannot write %s: %s", ctl->pwm_path, strerror(errno));
            return false;
        }
        return true;
    }

    case LG_PRIV_HELPER: {
        char enable_buf[LG_PATH_SCRATCH];
        char raw_buf[16];
        snprintf(raw_buf, sizeof(raw_buf), "%d", raw);

        const char *argv[6];
        int argc = 0;
        argv[argc++] = priv->helper_path;
        argv[argc++] = "--set";
        argv[argc++] = ctl->pwm_path;
        if (assert_enable) {
            /* One invocation carries both writes. */
            snprintf(enable_buf, sizeof(enable_buf), "%s", ctl->pwm_enable_path);
            argv[argc++] = "--enable";
            argv[argc++] = enable_buf;
        }
        argv[argc++] = raw_buf;
        argv[argc] = NULL;

        int rc = run_quiet(argv);
        if (rc != 0) {
            set_err(err, cap, "helper refused to write %s (status %d)", ctl->label, rc);
            return false;
        }
        return true;
    }

    case LG_PRIV_PKEXEC: {
        if (assert_enable) {
            const char *pre[] = {"/usr/bin/pkexec", "sh", "-c",
                                 "printf 1 > \"$1\"", "sh", ctl->pwm_enable_path, NULL};
            if (run_quiet(pre) != 0) {
                set_err(err, cap, "polkit refused to enter manual mode on %s", ctl->label);
                return false;
            }
        }
        char script[LG_PATH_SCRATCH + 32];
        snprintf(script, sizeof(script), "printf '%%s\\n' \"$1\" > \"$2\"");
        const char *argv[8] = {"/usr/bin/pkexec", "sh", "-c", script, "sh", NULL};
        char raw_buf[16];
        snprintf(raw_buf, sizeof(raw_buf), "%d", raw);
        argv[5] = raw_buf;
        argv[6] = ctl->pwm_path;
        argv[7] = NULL;
        if (run_quiet(argv) != 0) {
            set_err(err, cap, "polkit refused to write %s", ctl->label);
            return false;
        }
        return true;
    }

    case LG_PRIV_SUDO: {
        if (assert_enable) {
            const char *pre[] = {"/usr/bin/sudo", "-n", "sh", "-c", "printf 1 > \"$1\"", "sh",
                                 ctl->pwm_enable_path, NULL};
            if (run_quiet(pre) != 0) {
                set_err(err, cap, "sudo refused to enter manual mode on %s", ctl->label);
                return false;
            }
        }
        char script[128];
        snprintf(script, sizeof(script), "printf '%%s\\n' \"$1\" > \"$2\"");
        char raw_buf[16];
        snprintf(raw_buf, sizeof(raw_buf), "%d", raw);
        const char *argv[12] = {"/usr/bin/sudo", "-n", "sh", "-c", script, "sh", raw_buf,
                                ctl->pwm_path, NULL};
        if (run_quiet(argv) != 0) {
            set_err(err, cap, "sudo refused to write %s", ctl->label);
            return false;
        }
        return true;
    }

    case LG_PRIV_NONE:
    default:
        set_err(err, cap, "no privileged write mechanism is available");
        return false;
    }
}

/* Read a control's current raw duty straight from sysfs. */
static int read_raw_duty(const lg_control *ctl)
{
    if (ctl == NULL || ctl->kind != LG_CTRL_HWMON || ctl->pwm_path[0] == '\0') {
        return -1;
    }
    FILE *f = fopen(ctl->pwm_path, "re");
    if (f == NULL) {
        return -1;
    }
    char buf[32];
    int raw = -1;
    if (fgets(buf, sizeof(buf), f) != NULL) {
        raw = (int)strtol(buf, NULL, 10);
    }
    fclose(f);
    return raw;
}

lg_apply_result lg_control_apply_verified_n(const lg_priv *priv, const lg_control *ctl, int duty,
                                            int attempts, char *err, size_t cap)
{
    if (err != NULL && cap > 0) {
        err[0] = '\0';
    }
    if (ctl == NULL) {
        set_err(err, cap, "no control selected");
        return LG_APPLY_REJECTED;
    }
    if (priv == NULL || !priv->available) {
        set_err(err, cap, "no privileged write mechanism is available");
        return LG_APPLY_REJECTED;
    }

    if (ctl->kind == LG_CTRL_LIQUIDCTL) {
        if (!lg_liquidctl_set("liquidctl", ctl->liquidctl_name, duty, err, cap)) {
            return LG_APPLY_REJECTED;
        }
        return LG_APPLY_OK;
    }

    int want = lg_control_duty_to_raw(ctl, duty);
    lg_apply_result result = LG_APPLY_REJECTED;

    /*
     * The EC can hold its fan register set locked for up to a second while it
     * applies a previous change. nct6687's store_pwm gives up at that point,
     * discards the write, and still returns success, so the only way through is
     * to retry across roughly the same window the driver itself allows.
     */
    if (attempts < 1) {
        attempts = 1;
    }
    const unsigned kBackoffUs = 250 * 1000;

    for (int attempt = 0; attempt < attempts; attempt++) {
        if (!apply_hwmon(priv, ctl, want, err, cap)) {
            if (attempt + 1 < attempts) {
                usleep(kBackoffUs);
                continue;
            }
            return LG_APPLY_REJECTED;
        }

        int got = read_raw_duty(ctl);
        if (got < 0) {
            /* Cannot verify, so trust the write. */
            result = LG_APPLY_OK;
            break;
        }
        if (abs(got - want) <= 1) {
            result = LG_APPLY_OK;
            break;
        }
        result = LG_APPLY_UNRESPONSIVE;
        if (attempt + 1 < attempts) {
            usleep(kBackoffUs);
        }
    }

    if (result == LG_APPLY_UNRESPONSIVE) {
        int got = read_raw_duty(ctl);
        set_err(err, cap,
                "%s accepted the write but stayed at %d instead of %d; the controller "
                "appears to be holding this channel itself",
                ctl->label, got, want);
    }
    return result;
}

lg_apply_result lg_control_apply_verified(const lg_priv *priv, const lg_control *ctl, int duty,
                                          char *err, size_t cap)
{
    return lg_control_apply_verified_n(priv, ctl, duty, 4, err, cap);
}

bool lg_control_apply(const lg_priv *priv, const lg_control *ctl, int duty, char *err, size_t cap)
{
    return lg_control_apply_verified(priv, ctl, duty, err, cap) != LG_APPLY_REJECTED;
}

bool lg_control_handback(const lg_priv *priv, const lg_control *ctl, char *err, size_t cap)
{
    if (ctl == NULL || !ctl->has_enable || ctl->pwm_enable_path[0] == '\0') {
        return true; /* nothing to hand back */
    }
    if (priv == NULL || !priv->available) {
        set_err(err, cap, "no privileged write mechanism is available");
        return false;
    }

    /*
     * Prefer the mode observed before this application started, so the channel
     * is left exactly as found. Fall back to 2, the ABI value for "the
     * controller runs its own curve", when the original was never recorded. The
     * nct6687 driver also accepts the legacy 99 and normalises it to 2.
     */
    char mode_buf[16];
    /*
     * The helper only accepts 1 or 2, and the driver rejects anything else.
     * A recorded 0 means the driver had not initialised the channel; 2 is the
     * equivalent "the controller decides" state, so that is what to write.
     */
    int want_mode = ctl->enable_initial;
    if (want_mode != 1) {
        want_mode = 2;
    }
    snprintf(mode_buf, sizeof(mode_buf), "%d", want_mode);

    switch (priv->mode) {
    case LG_PRIV_DIRECT:
        return write_sysfs(ctl->pwm_enable_path, ctl->enable_initial == 1 ? 1 : 2);

    case LG_PRIV_HELPER: {
        /* --mode, not --set: pwm_enable is a mode selector, and the helper
         * deliberately refuses to treat it as a duty value. */
        const char *argv[5] = {priv->helper_path, "--mode", ctl->pwm_enable_path, mode_buf, NULL};
        if (run_quiet(argv) != 0) {
            set_err(err, cap, "helper refused to hand back %s", ctl->label);
            return false;
        }
        return true;
    }

    case LG_PRIV_PKEXEC: {
        const char *argv[8] = {"/usr/bin/pkexec", "sh", "-c",
                               "printf '%s\\n' \"$1\" > \"$2\"", "sh", "2", ctl->pwm_enable_path,
                               NULL};
        if (run_quiet(argv) != 0) {
            set_err(err, cap, "polkit refused to hand back %s", ctl->label);
            return false;
        }
        return true;
    }

    case LG_PRIV_SUDO: {
        const char *argv[12] = {"/usr/bin/sudo", "-n", "sh", "-c",
                                "printf '%s\\n' \"$1\" > \"$2\"", "sh", mode_buf,
                                ctl->pwm_enable_path, NULL};
        if (run_quiet(argv) != 0) {
            set_err(err, cap, "sudo refused to hand back %s", ctl->label);
            return false;
        }
        return true;
    }

    default:
        set_err(err, cap, "no privileged write mechanism is available");
        return false;
    }
}

bool lg_control_full_speed(const lg_priv *priv, const lg_control *ctl, char *err, size_t cap)
{
    return lg_control_apply(priv, ctl, 100, err, cap);
}

void lg_control_refresh(lg_control *ctl)
{
    if (ctl == NULL) {
        return;
    }

    char buf[32];
    if (ctl->kind == LG_CTRL_HWMON && ctl->pwm_path[0] != '\0') {
        FILE *f = fopen(ctl->pwm_path, "re");
        if (f != NULL) {
            if (fgets(buf, sizeof(buf), f) != NULL) {
                ctl->duty_raw = (int)strtol(buf, NULL, 10);
                if (ctl->duty_raw >= 0) {
                    int lo = ctl->driver_min >= 0 ? ctl->driver_min : 0;
                    int hi = ctl->driver_max >= 0 ? ctl->driver_max : 255;
                    if (ctl->duty_raw < lo) {
                        ctl->duty_raw = lo;
                    }
                    if (ctl->duty_raw > hi) {
                        ctl->duty_raw = hi;
                    }
                    ctl->duty = (ctl->duty_raw * 100 + 127) / 255;
                }
            }
            fclose(f);
        }
        if (ctl->has_enable && ctl->pwm_enable_path[0] != '\0') {
            FILE *e = fopen(ctl->pwm_enable_path, "re");
            if (e != NULL) {
                if (fgets(buf, sizeof(buf), e) != NULL) {
                    ctl->enable_manual = (strtol(buf, NULL, 10) == 1);
                }
                fclose(e);
            }
        }
    }

    if (ctl->has_fan_pair && ctl->fan_path[0] != '\0') {
        FILE *f = fopen(ctl->fan_path, "re");
        if (f != NULL) {
            if (fgets(buf, sizeof(buf), f) != NULL) {
                ctl->fan_rpm = (int)strtol(buf, NULL, 10);
            }
            fclose(f);
        }
    }
}
