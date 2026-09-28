/*
 * liquidgui - AIO and motherboard fan control.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lg_config.h"
#include "lg_control.h"
#include "lg_dump.h"
#include "lg_hwmon.h"
#include "lg_daemon.h"
#include "lg_ui.h"

#ifndef LG_VERSION
#define LG_VERSION "2.0.0"
#endif

#define UNUSED_ARG(x) (void)(x)

/* How often the worker is asked for a fresh pass, in milliseconds. */
#define REFRESH_MS 3000
#define REFRESH_FIRST_MS 250

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "\n"
           "  --dump-detect      print discovered sensors and controls as JSON\n"
           "  --dump-config      print the resolved configuration and exit\n"
           "  --hwmon-root DIR   scan an alternative sysfs tree (for testing)\n"
           "  --no-liquidctl     do not probe liquidctl for HID-only AIOs\n"
           "  --no-apply         start with automatic curve application off\n"
           "  --theme MODE       force the colour scheme: light or dark\n"
           "  --screenshot PATH  render the window to a PNG and exit\n"
           "  --daemon           apply curves with no window, until stopped\n"
           "  --version          print version and exit\n"
           "  --help             print this help and exit\n",
           argv0);
}

/*
 * Signal handling.
 *
 * A cooling controller that leaves fans pinned because the window was closed is
 * worse than useless, so SIGINT/SIGTERM/SIGHUP shut the application down
 * through the same orderly path as a window close.
 *
 * The callbacks are registered with g_unix_signal_add rather than sigaction.
 * GLib delivers them from the main loop through a self-pipe, so the shutdown
 * runs on the main thread where it can join the worker and then write. A raw
 * handler cannot do that: it would fork and exec the privileged helper and
 * allocate, while the worker thread is live, which is not async-signal-safe and
 * silently left the fans pinned on SIGTERM.
 */
static lg_ui *g_ui = NULL;
static gboolean g_shutdown_requested = FALSE;

static gboolean on_shutdown_idle(gpointer data);

/*
 * The GLib signal callback only flags and schedules; the shutdown itself runs
 * from the main loop so it cannot re-enter the handler.
 */
static gboolean on_terminate_signal(gpointer data)
{
    UNUSED_ARG(data);
    if (!g_shutdown_requested) {
        g_shutdown_requested = TRUE;
        g_idle_add(on_shutdown_idle, NULL);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_shutdown_idle(gpointer data)
{
    UNUSED_ARG(data);
    lg_ui *ui = g_ui;
    g_shutdown_requested = FALSE;
    if (ui != NULL) {
        lg_ui_shutdown(ui);
    }
    return G_SOURCE_REMOVE;
}

static void install_signal_handlers(void)
{
    g_unix_signal_add(SIGINT, on_terminate_signal, NULL);
    g_unix_signal_add(SIGTERM, on_terminate_signal, NULL);
    g_unix_signal_add(SIGHUP, on_terminate_signal, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* Periodic tick: ask the worker for a pass, then adopt whatever it produced. */
static gboolean on_tick(gpointer data)
{
    lg_ui *ui = data;
    lg_ui_poll(ui);
    lg_ui_request(ui);
    return G_SOURCE_CONTINUE;
}

/* After the first fast poll, drop to the steady cadence. */
static gboolean on_slow_tick(gpointer data)
{
    lg_ui *ui = data;
    lg_ui_poll(ui);
    lg_ui_request(ui);
    g_timeout_add(REFRESH_MS, on_tick, ui);
    return G_SOURCE_REMOVE;
}

static int run_gui(lg_ui_config *ucfg, const char *screenshot)
{
    /*
     * Ask GTK for a light or dark base theme before any widget exists. Doing it
     * later forces a theme reload, which rebuilds TreeView headers and loses
     * them.
     */
    {
        GtkSettings *settings = gtk_settings_get_default();
        if (settings != NULL) {
            g_object_set(settings, "gtk-application-prefer-dark-theme",
                         (ucfg->theme == LG_THEME_DARK), NULL);
        }
    }

    GtkWidget *window = lg_ui_new(ucfg);
    g_ui = (lg_ui *)g_object_get_data(G_OBJECT(window), "lg-ui");
    if (g_ui == NULL) {
        fprintf(stderr, "failed to initialise the interface\n");
        return 1;
    }

    install_signal_handlers();

    /*
     * Poll promptly at first so the window is populated as soon as possible,
     * then settle into the steady refresh cadence. The worker owns the actual
     * sysfs work; this only adopts whatever it has published.
     */
    g_timeout_add(REFRESH_FIRST_MS, on_slow_tick, g_ui);

    gtk_widget_show_all(window);
    if (screenshot != NULL) {
        lg_ui_screenshot_and_quit(g_ui, screenshot);
    }
    gtk_main();
    g_ui = NULL;
    return 0;
}

int main(int argc, char **argv)
{
    lg_discover_opts opts;
    lg_discover_opts_init(&opts);

    bool dump_detect = false;
    bool dump_config = false;
    bool no_apply = false;
    const char *root_override = NULL;
    bool no_liquidctl = false;
    const char *screenshot = NULL;
    const char *theme = NULL;
    bool daemon = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-detect") == 0) {
            dump_detect = true;
        } else if (strcmp(argv[i], "--dump-config") == 0) {
            dump_config = true;
        } else if (strcmp(argv[i], "--no-liquidctl") == 0) {
            no_liquidctl = true;
        } else if (strcmp(argv[i], "--no-apply") == 0) {
            no_apply = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon = true;
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme = argv[++i];
        } else if (strcmp(argv[i], "--hwmon-root") == 0 && i + 1 < argc) {
            root_override = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("liquidgui %s\n", LG_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (root_override != NULL) {
        opts.root = root_override;
    }
    if (no_liquidctl) {
        opts.use_liquidctl = false;
    }

    lg_snapshot snap;
    if (!lg_discover(&snap, &opts)) {
        fprintf(stderr, "sensor discovery failed\n");
        return 1;
    }

    if (dump_detect) {
        char *json = lg_snapshot_to_json(&snap, 0);
        if (json == NULL) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        fputs(json, stdout);
        free(json);
        return 0;
    }

    lg_config config;
    lg_config_load(&config, &snap, opts.root);
    if (no_apply) {
        config.auto_apply = false;
    }

    if (dump_config) {
        char *json = lg_config_to_json(&config);
        if (json != NULL) {
            fputs(json, stdout);
            free(json);
        }
        return 0;
    }

    /*
     * Headless curve application, before any GTK call.
     *
     * This has to come before gtk_init_check() deliberately: the point is to run
     * on a machine with no session, where initialising GTK would either fail or
     * insist on a display it will never get. The interface can be opened later
     * on the same config, and the daemon owns the curves until it stops.
     */
    if (daemon) {
        lg_daemon_state st;
        lg_daemon_state_init(&st, &config);
        st.root_override = opts.root;
        st.no_liquidctl = opts.use_liquidctl ? false : true;
        lg_daemon_install_signal_handlers();
        printf("liquidgui %s --daemon, source %s\n", LG_VERSION,
               isnan(snap.cpu_temp) ? "unavailable" : "cpu");
        fflush(stdout);
        lg_daemon_run(&st);
        printf("stopped after %lu pass(es)\n", st.passes);
        lg_config_save(&config);
        return 0;
    }

    /* Headless summary, useful over ssh and for scripting. */
    if (!gtk_init_check(NULL, NULL)) {
        printf("liquidgui %s\n", LG_VERSION);
        printf("no display available; showing a summary instead\n\n");
        printf("%-10s %-24s %-20s %-7s %s\n", "KIND", "CONTROL", "KEY", "DUTY", "RPM");
        for (size_t i = 0; i < snap.ncontrols; i++) {
            const lg_control *c = &snap.controls[i];
            char duty[16];
            char rpm[16];
            lg_control_format_duty(c, duty, sizeof(duty));
            if (c->has_fan_pair && c->fan_rpm >= 0) {
                snprintf(rpm, sizeof(rpm), "%d", c->fan_rpm);
            } else {
                snprintf(rpm, sizeof(rpm), "--");
            }
            printf("%-10s %-24s %-20s %-7s %s\n", c->is_aio ? "AIO" : "header", c->label,
                   c->key, duty, rpm);
        }
        if (snap.notes[0] != '\0') {
            printf("\nnotes: %s\n", snap.notes);
        }
        printf("\nconfig: %s\n", lg_config_path());
        return 0;
    }

    if (theme != NULL) {
        if (strcmp(theme, "light") == 0) {
            config.theme_light = true;
        } else if (strcmp(theme, "dark") == 0) {
            config.theme_light = false;
        } else {
            fprintf(stderr, "--theme expects 'light' or 'dark', got '%s'\n", theme);
            return 2;
        }
    }

    lg_ui_config ucfg;
    ucfg.config = config;
    ucfg.snapshot = snap;
    ucfg.theme = config.theme_light ? LG_THEME_LIGHT : LG_THEME_DARK;

    int rc = run_gui(&ucfg, screenshot);
    lg_config_save(&config);
    return rc;
}
