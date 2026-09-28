/*
 * lg_daemon - headless curve application.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 *
 * The interface already applies curves: it has a worker thread that re-reads the
 * sensors and writes each control's duty on a timer. But that worker is driven
 * by the GTK main loop, so the curves only run while a window is open. On a
 * machine with no session -- a server, a headless install, or simply nobody
 * logged in at the moment -- nothing was applying anything, and the fan duties
 * in sysfs were whatever the last window happened to leave behind.
 *
 * This is the same loop without the window. Same discovery, same curve
 * evaluation, same write verification, same failsafe, same stall reporting. It
 * owns the whole job: there is no window to close and no separate daemon to
 * coordinate with, so what runs in the foreground is exactly what ran before.
 */

#include "lg_daemon.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_control.h"
#include "lg_curve.h"
#include "lg_hwmon.h"

/* Set from a signal handler, so it must be sig_atomic_t and nothing else. */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

void lg_daemon_state_init(lg_daemon_state *st, lg_config *config)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->config = config;
    st->interval_ms = 3000;
}

void lg_daemon_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* A disconnected pipe must not kill the process mid-write. */
    signal(SIGPIPE, SIG_IGN);
}

bool lg_daemon_should_stop(void)
{
    return g_stop != 0;
}

void lg_daemon_sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    /*
     * Interrupted by a signal, which is how a stop request arrives. Returning
     * early is correct here: the caller re-checks the stop flag and exits.
     */
    (void)nanosleep(&ts, NULL);
}

/*
 * Run one pass: rediscover, evaluate every enabled curve against the source
 * temperature, and write the result.
 *
 * Returns the number of controls written, and fills failed/unresponsive so the
 * caller can report rather than only count.
 */
int lg_daemon_pass(lg_daemon_state *st)
{
    lg_discover_opts opts;
    lg_discover_opts_init(&opts);
    if (st->root_override != NULL) {
        opts.root = st->root_override;
    }
    if (st->no_liquidctl) {
        opts.use_liquidctl = false;
    }

    lg_snapshot *snap = malloc(sizeof(*snap));
    if (snap == NULL) {
        return -1;
    }
    if (!lg_discover(snap, &opts)) {
        free(snap);
        return -1;
    }

    /* Carry per-channel knowledge across passes, matched on the stable key. */
    if (st->prev != NULL && st->prev->ncontrols > 0) {
        for (size_t i = 0; i < snap->ncontrols; i++) {
            for (size_t j = 0; j < st->prev->ncontrols; j++) {
                if (strcmp(st->prev->controls[j].key, snap->controls[i].key) != 0) {
                    continue;
                }
                if (snap->controls[i].enable_initial < 0) {
                    snap->controls[i].enable_initial = st->prev->controls[j].enable_initial;
                }
                if (st->prev->controls[j].responds == 0) {
                    snap->controls[i].responds = 0;
                }
                break;
            }
        }
    }

    const bool failsafe = st->config->failsafe_temp > 0 && !isnan(snap->cpu_temp) &&
                          snap->cpu_temp >= (double)st->config->failsafe_temp;
    /*
     * auto_apply selects the mode -- follow the curve, or hold a fixed speed --
     * and no longer gates whether anything is written at all. It used to do
     * both, which made manual mode impossible: turning it off to hold a fixed
     * speed also stopped every write, so the fan sat at whatever it was doing
     * before and looked like it was still tracking temperature.
     *
     * Pausing is the only thing that stops writes, and it is always explicit.
     */
    const bool do_apply = !st->config->paused;

    int applied = 0;
    int failed = 0;
    int unresponsive = 0;
    int stalled = 0;
    char message[192] = {0};

    for (size_t i = 0; i < snap->ncontrols; i++) {
        lg_control *c = &snap->controls[i];
        const lg_curve *curve = lg_config_curve(st->config, c);

        if (do_apply) {
            /*
             * Same resolver the interface uses, so a machine under the daemon
             * cools identically to one under the window.
             */
            int duty;
            if (!lg_curve_resolve_duty(curve, !st->config->auto_apply, failsafe,
                                       snap->cpu_temp, &duty)) {
                continue;
            }

            char err[192] = {0};
            /*
             * A channel already known to ignore writes gets a single try.
             * Repeating it on every pass would add seconds of latency for a
             * result already established.
             */
            const int attempts = (c->responds == 0) ? 1 : 4;
            lg_apply_result r =
                lg_control_apply_verified_n(&st->priv, c, duty, attempts, err, sizeof(err));
            if (r == LG_APPLY_OK) {
                applied++;
                c->duty = duty;
                c->responds = 1;
            } else if (r == LG_APPLY_UNRESPONSIVE) {
                unresponsive++;
                c->responds = 0;
                if (message[0] == '\0') {
                    snprintf(message, sizeof(message), "%s", err);
                }
            } else {
                failed++;
                if (message[0] == '\0') {
                    snprintf(message, sizeof(message), "%s", err);
                }
            }
        }

        lg_control_refresh(c);

        /*
         * Stall detection: a fan commanded to at least 20% that reports no rpm
         * for several passes is not spinning, and hiding that from the log
         * would make a dead fan look like a healthy one at low duty.
         */
        const bool spinning_requested = (c->duty >= st->config->stall_duty) &&
                                       st->config->stall_duty > 0 && c->has_fan_pair;
        if (spinning_requested && c->fan_rpm == 0) {
            if (st->stall_count[i] < st->config->stall_samples) {
                st->stall_count[i]++;
            }
            if (st->stall_count[i] >= st->config->stall_samples) {
                st->stalled++;
            }
        } else {
            st->stall_count[i] = 0;
        }
    }

    st->applied = applied;
    st->failed = failed;
    st->unresponsive = unresponsive;
    st->stalled = stalled;
    st->failsafe = failsafe;
    st->cpu_temp = snap->cpu_temp;
    st->passes++;
    if (message[0] != '\0') {
        snprintf(st->message, sizeof(st->message), "%s", message);
    }

    free(st->prev);
    st->prev = snap; /* kept for the next pass's per-channel carry-over */

    return applied;
}

/*
 * Restore every control we were driving to full speed and exit.
 *
 * The same reasoning as the interface's exit path: an over-speed fan is
 * reversible, a stopped one is not, and there is no longer a program running to
 * manage the curve. Handback is deliberately not attempted here -- writing
 * pwm_enable=2 is documented on nct6687 as releasing the channel to the
 * controller, but was measured on nzxt_kraken3 to zero both outputs and stop the
 * radiator fan.
 */
void lg_daemon_restore_on_exit(lg_daemon_state *st)
{
    if (st->prev == NULL) {
        return;
    }
    for (size_t i = 0; i < st->prev->ncontrols; i++) {
        lg_control *c = &st->prev->controls[i];
        if (st->config != NULL) {
            const lg_curve *curve = lg_config_curve(st->config, c);
            if (curve == NULL || !curve->enabled) {
                continue;
            }
        }
        char err[192] = {0};
        if (!lg_control_full_speed(&st->priv, c, err, sizeof(err))) {
            fprintf(stderr, "liquidgui: could not restore %s to full speed: %s\n", c->label, err);
        }
    }
}

/* Run until stopped. Returns the number of passes completed. */
int lg_daemon_run(lg_daemon_state *st)
{
    lg_priv_detect(&st->priv);

    if (!st->priv.available) {
        fprintf(stderr,
                "liquidgui: no privileged write mechanism found.\n"
                "            install it with: sudo make install-helper\n");
        return 1;
    }

    while (!lg_daemon_should_stop()) {
        if (lg_daemon_pass(st) < 0) {
            fprintf(stderr, "liquidgui: discovery failed, retrying\n");
        }
        for (long slept = 0; slept < st->interval_ms && !lg_daemon_should_stop(); slept += 100) {
            lg_daemon_sleep_ms(100);
        }
    }

    lg_daemon_restore_on_exit(st);
    return (int)st->passes;
}
