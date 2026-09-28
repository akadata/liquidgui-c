/*
 * lg_liquidctl - optional HID-backed AIO discovery and control.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_liquidctl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lg_json.h"

/*
 * Run a command, capturing stdout. Returns the exit status, or -1 when the
 * binary is absent. *out receives a malloc'd NUL-terminated buffer.
 */
static int run_capture(const char *const argv[], char **out, size_t *out_len)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: stdout to the pipe, stderr to /dev/null. */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(fds[1]);

    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        close(fds[0]);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
            /* reap */
        }
        return -1;
    }

    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap * 2;
            char *nbuf = realloc(buf, ncap);
            if (nbuf == NULL) {
                free(buf);
                close(fds[0]);
                return -1;
            }
            buf = nbuf;
            cap = ncap;
        }
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    close(fds[0]);
    buf[len] = '\0';

    *out = buf;
    if (out_len != NULL) {
        *out_len = len;
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

/* Find a telemetry entry by its liquidctl key, e.g. "pump duty". */
static const lg_json *find_status(const lg_json *device, const char *needle)
{
    const lg_json *status = lg_json_get(device, "status");
    for (size_t i = 0; i < lg_json_len(status); i++) {
        const char *key = lg_json_get_str(lg_json_at(status, i), "key", "");
        if (strcasecmp(key, needle) == 0) {
            return lg_json_at(status, i);
        }
    }
    return NULL;
}

bool lg_liquidctl_discover(lg_snapshot *snap, const char *liquidctl_bin)
{
    if (snap == NULL) {
        return false;
    }

    const char *bin = (liquidctl_bin != NULL) ? liquidctl_bin : "liquidctl";
    const char *argv[] = {bin, "--json", "status", NULL};

    char *out = NULL;
    int rc = run_capture(argv, &out, NULL);
    if (rc != 0 || out == NULL) {
        free(out);
        return false;
    }

    const char *err = NULL;
    lg_json_doc *doc = lg_json_parse(out, &err);
    free(out);
    if (doc == NULL) {
        return false;
    }

    const lg_json *root = lg_json_root(doc);
    bool found = false;

    for (size_t d = 0; d < lg_json_len(root); d++) {
        const lg_json *device = lg_json_at(root, d);
        const char *description = lg_json_get_str(device, "description", "AIO");

        /*
         * liquidctl reports fan and pump duty for one cooler. Map them onto
         * channels 0 and 1 to match the legacy liquidctl:pump / liquidctl:fan
         * key ordering, which listed the pump first.
         */
        static const struct {
            const char *duty_key;
            const char *channel;
            const char *label;
        } map[] = {
            {"Pump duty", "pump", "AIO pump"},
            {"Fan duty", "fan", "AIO fan"},
        };

        for (size_t m = 0; m < sizeof(map) / sizeof(map[0]); m++) {
            const lg_json *duty = find_status(device, map[m].duty_key);
            if (duty == NULL) {
                continue;
            }
            if (snap->ncontrols >= LG_MAX_CONTROLS) {
                break;
            }

            lg_control *c = &snap->controls[snap->ncontrols];
            memset(c, 0, sizeof(*c));
            c->kind = LG_CTRL_LIQUIDCTL;
            c->duty = (int)lg_py_round(lg_json_get_num(duty, "value", -1.0));
            c->duty_raw = -1;
            c->driver_min = -1;
            c->driver_max = -1;
            c->fan_rpm = -1;
            c->responds = 1;
            c->is_aio = true;
            c->channel = (strcmp(map[m].channel, "pump") == 0) ? 0 : 1;

            snprintf(c->chip, sizeof(c->chip), "%s", description);
            snprintf(c->label, sizeof(c->label), "%s", map[m].label);
            snprintf(c->liquidctl_name, sizeof(c->liquidctl_name), "%s", map[m].channel);
            lg_control_make_key(c->key, sizeof(c->key), c->kind, NULL, c->chip, c->channel);

            snap->ncontrols++;
            found = true;
        }
    }

    lg_json_doc_free(doc);
    return found;
}

bool lg_liquidctl_set(const char *liquidctl_bin, const char *channel, int duty_percent, char *err,
                      size_t err_cap)
{
    if (err != NULL && err_cap > 0) {
        err[0] = '\0';
    }
    if (channel == NULL) {
        if (err != NULL) {
            snprintf(err, err_cap, "no liquidctl channel");
        }
        return false;
    }

    const char *bin = (liquidctl_bin != NULL) ? liquidctl_bin : "liquidctl";
    char duty_buf[16];
    snprintf(duty_buf, sizeof(duty_buf), "%d", duty_percent);

    const char *argv[] = {bin, "set", channel, "speed", duty_buf, NULL};
    char *out = NULL;
    int rc = run_capture(argv, &out, NULL);
    free(out);

    if (rc != 0) {
        if (err != NULL) {
            snprintf(err, err_cap, "liquidctl set %s failed (status %d)", channel, rc);
        }
        return false;
    }
    return true;
}
