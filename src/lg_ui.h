/*
 * lg_ui - GTK3 interface.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_UI_H
#define LG_UI_H

#include <gtk/gtk.h>

#include "lg_config.h"
#include "lg_model.h"

typedef struct {
    lg_config config;
    lg_snapshot snapshot;
} lg_ui_config;

typedef struct lg_ui lg_ui;

/* Build the window. Returns the top-level widget. */
GtkWidget *lg_ui_new(lg_ui_config *cfg);

/*
 * Adopt any snapshot the worker has published and refresh the views. Must be
 * called on the GTK main thread, typically from a periodic timeout.
 */
void lg_ui_poll(lg_ui *ui);

/* Ask the worker for a fresh discovery pass. */
void lg_ui_request(lg_ui *ui);

/* Access the live configuration, for saving and for the exit handler. */
lg_ui_config *lg_ui_config_of(lg_ui *ui);

/*
 * Save the window to a PNG, then leave the main loop. Used by --screenshot so
 * the interface can be captured for bug reports without depending on an
 * external screenshot tool being installed.
 */
void lg_ui_screenshot_and_quit(lg_ui *ui, const char *path);

/*
 * Return cooling to a safe state according to the configured exit mode.
 */
void lg_ui_restore_on_exit(lg_ui *ui);

/*
 * Orderly shutdown: stop the worker thread so nothing can write concurrently,
 * restore cooling, save the configuration, then leave the main loop.
 *
 * This must run on the GTK main thread rather than inside a signal handler.
 * The restore path forks and execs the privileged helper and may allocate, and
 * doing that from a signal handler while the worker thread is live is neither
 * async-signal-safe nor safe against the allocator's lock. An earlier version
 * did exactly that and silently left the fans pinned on SIGTERM.
 */
void lg_ui_shutdown(lg_ui *ui);

#endif /* LG_UI_H */
