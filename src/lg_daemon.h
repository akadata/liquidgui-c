/*
 * lg_daemon - headless curve application.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */
#ifndef LG_DAEMON_H
#define LG_DAEMON_H

#include <stdbool.h>
#include <stddef.h>

#include "lg_config.h"
#include "lg_control.h"
#include "lg_hwmon.h"

/*
 * State for a headless run.
 *
 * The interface holds the equivalent in its lg_ui struct; this is the same set
 * of fields with nothing GTK in it. prev keeps the previous discovery so
 * per-channel knowledge -- which channels ignore writes -- survives between
 * passes instead of being relearned every interval.
 */
typedef struct {
    lg_config *config;         /* curves and policy, not owned */
    lg_priv priv;              /* the privilege bridge */
    lg_snapshot *prev;         /* previous discovery, owned; NULL on the first pass */
    long interval_ms;          /* between passes */
    const char *root_override; /* hwmon root, or NULL for the real one */
    bool no_liquidctl;

    /* Results of the most recent pass, for reporting. */
    int applied;
    int failed;
    int unresponsive;
    int stalled;
    bool failsafe;
    double cpu_temp;
    unsigned long passes;
    char message[192];

    int stall_count[LG_MAX_CONTROLS];
} lg_daemon_state;

/* Prepare state with defaults; call before filling in the overrides. */
void lg_daemon_state_init(lg_daemon_state *st, lg_config *config);

/* Install SIGINT/SIGTERM/SIGHUP handlers and ignore SIGPIPE. */
void lg_daemon_install_signal_handlers(void);

/* Sleep, returning early if a signal arrives. */
void lg_daemon_sleep_ms(long ms);

/* True once a stop signal has arrived. */
bool lg_daemon_should_stop(void);

/* One discovery-and-apply pass. Returns the number of controls written, or -1. */
int lg_daemon_pass(lg_daemon_state *st);

/* Write full speed to every enabled control. Called on exit. */
void lg_daemon_restore_on_exit(lg_daemon_state *st);

/* Loop until stopped, then restore. Returns the number of passes completed. */
int lg_daemon_run(lg_daemon_state *st);

#endif /* LG_DAEMON_H */
