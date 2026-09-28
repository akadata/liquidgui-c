/*
 * lg_ui - GTK3 interface for curve editing, control, and monitoring.
 *
 * Layout keeps the shape of the previous Tk interface: a control list on the
 * left, the Bezier editor on the right, and a sensor readout beneath. What
 * changed is that the editor draws with cairo rather than canvas items, the
 * controls are a TreeView with per-channel columns instead of a bare Listbox,
 * and all sysfs work happens on a worker thread so the window stops freezing
 * while a control is written.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_ui.h"

#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lg_control.h"
#include "lg_curve.h"
#include "lg_hwmon.h"
#include "lg_theme.h"

typedef struct lg_ui lg_ui;

struct lg_ui {
    GtkWidget *window;
    GtkWidget *header_temp;
    GtkWidget *header_coolant;
    GtkWidget *header_note;
    GtkWidget *banner;
    GtkWidget *banner_label;

    GtkWidget *controls_view;
    GtkListStore *controls_store;

    GtkWidget *curve_area;
    GtkWidget *curve_info;
    GtkWidget *detail_label;

    GtkWidget *notebook;
    GtkWidget *sensor_views[LG_SENSOR_CLASS_COUNT];
    GtkListStore *sensor_stores[LG_SENSOR_CLASS_COUNT];

    GtkWidget *status_label;
    GtkWidget *priv_label;

    GtkWidget *auto_check;
    GtkWidget *pause_check;

    lg_ui_config cfg;
    lg_priv priv;

    /*
     * Guards lg_config.config, which the worker thread and the GTK main thread
     * both reach. lg_config_curve() appends to the entry array when it meets a
     * control it has not seen, so concurrent access from the two threads is a
     * real race and not merely untidy. Readers copy what they need under the
     * lock and then work outside it, so the critical section stays short.
     */
    GMutex config_lock;

    /* Worker state. The snapshot is owned by the UI thread; the worker posts
     * a request and the main loop hands results back through this queue. */
    GMutex worker_lock;
    GCond worker_cond;
    gboolean worker_requested;
    gboolean worker_quit;
    GThread *worker;
    lg_snapshot pending;
    gboolean pending_valid;
    char pending_message[256];
    int pending_applied;
    int pending_failed;
    int pending_unresponsive;
    int pending_stalled;
    gboolean pending_failsafe;
    double pending_failsafe_temp;

    /* Curve editing. */
    int drag_index;
    bool dirty;
    bool dark_mode;
    GtkCssProvider *css_provider;

    /* Stall bookkeeping, one counter per control. */
    int stall_count[LG_MAX_CONTROLS];
};

static gpointer ui_worker_entry(gpointer data);

/* Mark unused GTK callback parameters without disabling the warning globally. */
#define UNUSED(x) (void)(x)

/* ------------------------------------------------------------- appearance */

static void apply_theme(lg_ui *ui);

/* ---------------------------------------------------------------- helpers */

static lg_control *selected_control(lg_ui *ui)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->controls_view));
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    gint index = -1;

    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) {
        return NULL;
    }
    gtk_tree_model_get(model, &iter, 0, &index, -1);
    if (index < 0 || (size_t)index >= ui->cfg.snapshot.ncontrols) {
        return NULL;
    }
    return &ui->cfg.snapshot.controls[index];
}

static const lg_curve *selected_curve(lg_ui *ui)
{
    lg_control *c = selected_control(ui);
    if (c == NULL) {
        return NULL;
    }
    return lg_config_curve(&ui->cfg.config, c);
}

static void set_status(lg_ui *ui, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(ui->status_label), buf);
}

/* Ask the worker thread to refresh and optionally re-apply. */
static void request_refresh(lg_ui *ui)
{
    g_mutex_lock(&ui->worker_lock);
    ui->worker_requested = TRUE;
    g_cond_signal(&ui->worker_cond);
    g_mutex_unlock(&ui->worker_lock);
}

static void request_apply(lg_ui *ui)
{
    g_mutex_lock(&ui->worker_lock);
    ui->worker_requested = TRUE;
    g_cond_signal(&ui->worker_cond);
    g_mutex_unlock(&ui->worker_lock);
}

/* --------------------------------------------------------- control store */

static void rebuild_controls(lg_ui *ui)
{
    const lg_snapshot *snap = &ui->cfg.snapshot;

    char *previous = NULL;
    lg_control *sel = selected_control(ui);
    if (sel != NULL) {
        previous = g_strdup(sel->key);
    }

    gtk_list_store_clear(ui->controls_store);

    /* AIO coolers first, then motherboard headers, so the channels that
     * matter most are not buried below eight identical rows. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < snap->ncontrols; i++) {
            const lg_control *c = &snap->controls[i];
            if ((pass == 0) != (c->is_aio)) {
                continue;
            }

            /*
             * lg_config_curve() appends to the entry array for a control it
             * has not seen yet, so it is called under the lock the worker also
             * takes. The curve contents themselves are only ever written by
             * this thread, so they can be read after the lock is released.
             */
            g_mutex_lock(&ui->config_lock);
            const lg_curve *curve = lg_config_curve(&ui->cfg.config, c);
            g_mutex_unlock(&ui->config_lock);

            char duty[16];
            lg_control_format_duty(c, duty, sizeof(duty));
            char rpm[16];
            if (c->has_fan_pair && c->fan_rpm >= 0) {
                snprintf(rpm, sizeof(rpm), "%d", c->fan_rpm);
            } else {
                snprintf(rpm, sizeof(rpm), "--");
            }

            /* Predicted duty at the current source temperature. */
            char predicted[16] = "--";
            if (curve != NULL && !isnan(snap->cpu_temp)) {
                int duty_pct = lg_curve_duty(curve, snap->cpu_temp);
                if (curve->min_duty > duty_pct) {
                    duty_pct = curve->min_duty;
                }
                snprintf(predicted, sizeof(predicted), "%d%%", duty_pct);
            }

            GtkTreeIter iter;
            gtk_list_store_append(ui->controls_store, &iter);
            gtk_list_store_set(ui->controls_store, &iter,
                               0, (int)i,
                               1, c->label,
                               2, c->chip,
                               3, duty,
                               4, predicted,
                               5, rpm,
                               6, c->responds < 0
                                      ? (c->has_enable ? (c->enable_manual ? "manual" : "auto")
                                                       : "n/a")
                                      : (c->responds ? (c->has_enable
                                                           ? (c->enable_manual ? "manual" : "auto")
                                                           : "n/a")
                                                     : "not responding"),
                               7, (curve != NULL && curve->enabled) ? "on" : "off",
                               8, c->key,
                               -1);
        }
    }

    if (previous != NULL) {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->controls_store), &iter);
        while (valid) {
            gchar *key = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(ui->controls_store), &iter, 8, &key, -1);
            if (key != NULL && strcmp(key, previous) == 0) {
                GtkTreeSelection *pick =
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->controls_view));
                gtk_tree_selection_select_iter(pick, &iter);
                break;
            }
            g_free(key);
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->controls_store), &iter);
        }
        g_free(previous);
    } else if (ui->cfg.config.selected_key[0] != '\0') {
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->controls_store), &iter);
        while (valid) {
            gchar *key = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(ui->controls_store), &iter, 8, &key, -1);
            if (key != NULL && strcmp(key, ui->cfg.config.selected_key) == 0) {
                GtkTreeSelection *pick =
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->controls_view));
                gtk_tree_selection_select_iter(pick, &iter);
                break;
            }
            g_free(key);
            valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->controls_store), &iter);
        }
    }
}

/* --------------------------------------------------------- sensor stores */

/* Ordering weight for a sensor: lower sorts first. */
static int sensor_rank(const lg_sensor *s)
{
    if (s->cls == LG_SENSOR_TEMP) {
        if (strstr(s->label, "Package") != NULL) {
            return 0;
        }
        if (strstr(s->label, "Coolant") != NULL || strstr(s->label, "Liquid") != NULL) {
            return 1;
        }
    }
    if (s->cls == LG_SENSOR_FAN && s->valid && s->value > 0.0) {
        return 2; /* spinning fans before stopped ones */
    }
    return 3;
}

static int sensor_compare(const void *a, const void *b)
{
    const lg_sensor *x = *(const lg_sensor *const *)a;
    const lg_sensor *y = *(const lg_sensor *const *)b;

    if (x->secondary != y->secondary) {
        return x->secondary ? 1 : -1;
    }
    int rx = sensor_rank(x);
    int ry = sensor_rank(y);
    if (rx != ry) {
        return rx - ry;
    }
    int c = strcmp(x->chip, y->chip);
    if (c != 0) {
        return c;
    }
    return (x->index > y->index) - (x->index < y->index);
}

static void rebuild_sensors(lg_ui *ui)
{
    const lg_snapshot *snap = &ui->cfg.snapshot;

    const lg_sensor **sorted = malloc(sizeof(*sorted) * (snap->nsensors + 1));
    if (sorted == NULL) {
        return;
    }
    size_t nsorted = 0;
    for (size_t i = 0; i < snap->nsensors; i++) {
        sorted[nsorted++] = &snap->sensors[i];
    }
    qsort(sorted, nsorted, sizeof(*sorted), sensor_compare);

    for (int cls = 0; cls < LG_SENSOR_CLASS_COUNT; cls++) {
        GtkListStore *store = ui->sensor_stores[cls];
        if (store == NULL) {
            continue;
        }
        gtk_list_store_clear(store);

        size_t emitted = 0;
        for (size_t k = 0; k < nsorted; k++) {
            {
                const lg_sensor *s = sorted[k];
                if ((int)s->cls != cls) {
                    continue;
                }
                emitted++;

                char value[64];
                lg_sensor_format(s, value, sizeof(value));

                char range[96];
                if (s->has_min && s->has_max) {
                    snprintf(range, sizeof(range), "%.3g .. %.3g", s->min, s->max);
                } else if (s->has_crit) {
                    snprintf(range, sizeof(range), "crit %.3g", s->crit);
                } else {
                    snprintf(range, sizeof(range), "--");
                }

                GtkTreeIter iter;
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter,
                                   0, s->label,
                                   1, s->chip,
                                   2, s->driver,
                                   3, value,
                                   4, range,
                                   5, s->valid ? "" : s->note,
                                   6, (int)(s - snap->sensors),
                                   -1);
            }
        }

        /*
         * An empty tab is indistinguishable from a broken one, so say why. This
         * matters most for current: hwmon overloads in*_input for both voltage
         * and current, and a board with no amperage channels has nothing to
         * show there however complete the support.
         */
        if (emitted == 0) {
            const char *why;
            switch ((lg_sensor_class)cls) {
            case LG_SENSOR_CURRENT:
                why = "no current channels are exposed by this hardware";
                break;
            case LG_SENSOR_POWER:
                why = "no power or energy counters are exposed by this hardware";
                break;
            case LG_SENSOR_TEMP:
                why = "no temperature channels were found";
                break;
            case LG_SENSOR_FAN:
                why = "no fan tachometers were found";
                break;
            case LG_SENSOR_VOLT:
                why = "no voltage rails were found";
                break;
            default:
                why = "nothing was found";
                break;
            }
            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                               0, "(none)",
                               1, "",
                               2, "",
                               3, "--",
                               4, "",
                               5, why,
                               6, -1,
                               -1);
        }
    }

    free((void *)sorted);
}

/* ------------------------------------------------------------ curve editor */

typedef struct {
    double left, right, top, bottom;
    double width, height;
} lg_plot_area;

static lg_plot_area plot_area(lg_ui *ui)
{
    GtkAllocation alloc;
    gtk_widget_get_allocation(ui->curve_area, &alloc);

    lg_plot_area a;
    a.left = 52.0;
    a.right = (double)MAX(alloc.width, 320) - 20.0;
    a.top = 16.0;
    a.bottom = (double)MAX(alloc.height, 200) - 30.0;
    a.width = a.right - a.left;
    a.height = a.bottom - a.top;
    return a;
}

/*
 * Colours for the curve editor. The widget draws with cairo rather than
 * picking them up from the theme, so it would otherwise stay dark in light
 * mode and read as a hole in the window.
 */
typedef struct {
    double bg[3];
    double grid[3];
    double axis_text[3];
    double border[3];
    double curve[3];
    double marker[4]; /* rgba: source temperature line */
    double point[3];
    double label[3];
    double text[3];
} lg_plot_palette;

static const lg_plot_palette PLOT_DARK = {
    {0.055, 0.062, 0.075},  /* bg */
    {0.14, 0.15, 0.18},    /* grid */
    {0.77, 0.78, 0.81},    /* axis text */
    {0.38, 0.41, 0.46},    /* border */
    {0.47, 0.75, 1.0},     /* curve */
    {0.30, 0.85, 0.65, 0.55},
    {1.0, 0.71, 0.33},     /* point */
    {0.94, 0.95, 0.96},    /* label */
    {0.85, 0.86, 0.88},    /* text */
};

static const lg_plot_palette PLOT_LIGHT = {
    {0.98, 0.985, 0.99},
    {0.85, 0.87, 0.90},
    {0.32, 0.34, 0.38},
    {0.60, 0.63, 0.68},
    {0.05, 0.37, 0.71},
    {0.05, 0.55, 0.36, 0.65},
    {0.85, 0.47, 0.05},
    {0.11, 0.11, 0.13},
    {0.11, 0.11, 0.13},
};

static gboolean curve_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    UNUSED(widget);
    lg_ui *ui = data;
    const lg_curve *curve = selected_curve(ui);
    lg_plot_area a = plot_area(ui);
    const lg_plot_palette *pal = ui->dark_mode ? &PLOT_DARK : &PLOT_LIGHT;

    /* Background. */
    cairo_set_source_rgb(cr, pal->bg[0], pal->bg[1], pal->bg[2]);
    cairo_paint(cr);

    if (curve == NULL || curve->npoints == 0) {
        cairo_set_source_rgb(cr, pal->text[0], pal->text[1], pal->text[2]);
        cairo_move_to(cr, 20, 20);
        cairo_show_text(cr, "No control selected");
        return FALSE;
    }

    /* The editor zooms to the curve's own temperature span. */
    double t0 = curve->points[0].temp;
    double t1 = curve->points[curve->npoints - 1].temp;
    if (t1 - t0 < 10.0) {
        t1 = t0 + 10.0;
    }

    /* Grid. */
    cairo_set_line_width(cr, 1.0);
    double step = MAX(2.0, floor((t1 - t0) / 5.0));
    for (double t = t0; t <= t1 + 0.001; t += step) {
        double x = a.left + ((t - t0) / (t1 - t0)) * a.width;
        cairo_set_source_rgb(cr, pal->grid[0], pal->grid[1], pal->grid[2]);
        cairo_move_to(cr, x, a.top);
        cairo_line_to(cr, x, a.bottom);

        char label[16];
        snprintf(label, sizeof(label), "%d", (int)llround(t));
        cairo_set_source_rgb(cr, pal->axis_text[0], pal->axis_text[1], pal->axis_text[2]);
        cairo_move_to(cr, x - 8, a.bottom + 16);
        cairo_show_text(cr, label);
    }

    for (int duty = 0; duty <= 100; duty += 20) {
        double y = a.top + ((100.0 - duty) / 100.0) * a.height;
        cairo_set_source_rgb(cr, pal->grid[0], pal->grid[1], pal->grid[2]);
        cairo_move_to(cr, a.left, y);
        cairo_line_to(cr, a.right, y);

        char label[16];
        snprintf(label, sizeof(label), "%d", duty);
        cairo_set_source_rgb(cr, pal->axis_text[0], pal->axis_text[1], pal->axis_text[2]);
        cairo_move_to(cr, 6, y + 4);
        cairo_show_text(cr, label);
    }

    cairo_set_source_rgb(cr, pal->border[0], pal->border[1], pal->border[2]);
    cairo_rectangle(cr, a.left, a.top, a.width, a.height);
    cairo_stroke(cr);

    /* Live source temperature marker. */
    if (!isnan(ui->cfg.snapshot.cpu_temp)) {
        double tt = ui->cfg.snapshot.cpu_temp;
        if (tt >= t0 && tt <= t1) {
            double x = a.left + ((tt - t0) / (t1 - t0)) * a.width;
            cairo_set_source_rgba(cr, pal->marker[0], pal->marker[1], pal->marker[2],
                                  pal->marker[3]);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, x, a.top);
            cairo_line_to(cr, x, a.bottom);
            cairo_stroke(cr);
        }
    }

    /* The curve itself, from the cached sample table. */
    size_t ns = lg_curve_build_samples((lg_curve *)curve);
    if (ns >= 2) {
        cairo_set_source_rgb(cr, pal->curve[0], pal->curve[1], pal->curve[2]);
        cairo_set_line_width(cr, 3.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (size_t i = 0; i < ns; i++) {
            double x = a.left + ((curve->sx[i] - t0) / (t1 - t0)) * a.width;
            double y = a.top + ((100.0 - curve->sy[i]) / 100.0) * a.height;
            if (i == 0) {
                cairo_move_to(cr, x, y);
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        cairo_stroke(cr);
    }

    /* Draggable points. */
    for (size_t i = 0; i < curve->npoints; i++) {
        int temp = curve->points[i].temp;
        int duty = curve->points[i].duty;
        double x = a.left + ((temp - t0) / (t1 - t0)) * a.width;
        double y = a.top + ((100.0 - duty) / 100.0) * a.height;

        if (i == (size_t)ui->drag_index) {
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_arc(cr, x, y, 7, 0, 2 * M_PI);
            cairo_stroke(cr);
        }

        cairo_set_source_rgb(cr, pal->point[0], pal->point[1], pal->point[2]);
        cairo_arc(cr, x, y, 5, 0, 2 * M_PI);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, pal->label[0], pal->label[1], pal->label[2]);
        char label[32];
        snprintf(label, sizeof(label), "%dC/%d%%", temp, duty);

        /*
         * Draw the label above the point normally, but flip it below when the
         * point sits near the top edge so it is not clipped by the widget.
         */
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);
        double lx = x - ext.width / 2.0;
        double ly = (y - 12.0 < a.top + ext.height) ? (y + ext.height + 6.0) : (y - 12.0);
        if (lx < a.left) {
            lx = a.left;
        }
        if (lx + ext.width > a.right) {
            lx = a.right - ext.width;
        }
        cairo_move_to(cr, lx, ly);
        cairo_show_text(cr, label);
    }

    return FALSE;
}

static void update_curve_labels(lg_ui *ui)
{
    lg_control *c = selected_control(ui);
    const lg_curve *curve = selected_curve(ui);
    if (c == NULL || curve == NULL) {
        gtk_label_set_text(GTK_LABEL(ui->curve_info), "Select a control to edit its curve.");
        return;
    }

    char current[16];
    lg_control_format_duty(c, current, sizeof(current));

    char predicted[16] = "n/a";
    if (!isnan(ui->cfg.snapshot.cpu_temp)) {
        int duty_pct = lg_curve_duty(curve, ui->cfg.snapshot.cpu_temp);
        if (curve->min_duty > duty_pct) {
            duty_pct = curve->min_duty;
        }
        snprintf(predicted, sizeof(predicted), "%d%%", duty_pct);
    }

    char temp_text[32] = "n/a";
    if (!isnan(ui->cfg.snapshot.cpu_temp)) {
        snprintf(temp_text, sizeof(temp_text), "%.1f C", ui->cfg.snapshot.cpu_temp);
    }

    char info[512];
    snprintf(info, sizeof(info),
             "%s / %s     current %s     predicted at %s: %s     minimum duty %d%%",
             c->chip, c->label, current, temp_text, predicted, curve->min_duty);
    gtk_label_set_text(GTK_LABEL(ui->curve_info), info);
}

static void refresh_curve(lg_ui *ui)
{
    update_curve_labels(ui);
    gtk_widget_queue_draw(ui->curve_area);
}

/* Canvas <-> curve space for the current zoom. */
static void canvas_to_point(lg_ui *ui, double cx, double cy, int *temp_out, int *duty_out)
{
    const lg_curve *curve = selected_curve(ui);
    lg_plot_area a = plot_area(ui);

    double t0 = 0, t1 = 90;
    if (curve != NULL && curve->npoints > 0) {
        t0 = curve->points[0].temp;
        t1 = curve->points[curve->npoints - 1].temp;
        if (t1 - t0 < 10.0) {
            t1 = t0 + 10.0;
        }
    }

    double t = t0 + ((cx - a.left) / a.width) * (t1 - t0);
    double duty = 100.0 - ((cy - a.top) / a.height) * 100.0;

    *temp_out = (int)llround(t);
    *duty_out = (int)llround(duty);

    if (*duty_out < 0) {
        *duty_out = 0;
    }
    if (*duty_out > 100) {
        *duty_out = 100;
    }
}

static gboolean curve_button_cb(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    UNUSED(w);
    lg_ui *ui = data;
    const lg_curve *curve = selected_curve(ui);
    if (curve == NULL) {
        return FALSE;
    }

    lg_plot_area a = plot_area(ui);
    double t0 = curve->points[0].temp;
    double t1 = curve->points[curve->npoints - 1].temp;
    if (t1 - t0 < 10.0) {
        t1 = t0 + 10.0;
    }

    int best = -1;
    double best_dist = 14.0;
    for (size_t i = 0; i < curve->npoints; i++) {
        double x = a.left + ((curve->points[i].temp - t0) / (t1 - t0)) * a.width;
        double y = a.top + ((100.0 - curve->points[i].duty) / 100.0) * a.height;
        double dx = ev->x - x;
        double dy = ev->y - y;
        double d = sqrt(dx * dx + dy * dy);
        if (d < best_dist) {
            best_dist = d;
            best = (int)i;
        }
    }

    if (ev->type == GDK_BUTTON_PRESS) {
        if (ev->button == 3 && best >= 0) {
            /* Right click removes a point, keeping at least two. */
            const lg_curve *cv = selected_curve(ui);
            if (cv != NULL && cv->npoints > 2) {
                lg_curve *mut = (lg_curve *)cv;
                for (size_t i = (size_t)best; i + 1 < mut->npoints; i++) {
                    mut->points[i] = mut->points[i + 1];
                }
                mut->npoints--;
                mut->samples_valid = FALSE;
                ui->dirty = TRUE;
                refresh_curve(ui);
            }
            return TRUE;
        }
        if (ev->button == 1) {
            ui->drag_index = best;
            gtk_widget_queue_draw(ui->curve_area);
            return TRUE;
        }
    }

    if (ev->type == GDK_BUTTON_RELEASE && ev->button == 1) {
        ui->drag_index = -1;
        if (ui->dirty) {
            lg_config_save(&ui->cfg.config);
            ui->dirty = FALSE;
        }
        gtk_widget_queue_draw(ui->curve_area);
        return TRUE;
    }

    return FALSE;
}

static gboolean curve_motion_cb(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    UNUSED(w);
    lg_ui *ui = data;
    if (ui->drag_index < 0) {
        return FALSE;
    }

    lg_control *c = selected_control(ui);
    const lg_curve *view = selected_curve(ui);
    if (c == NULL || view == NULL) {
        return FALSE;
    }
    lg_curve *curve = (lg_curve *)view;

    int temp = 0;
    int duty = 0;
    canvas_to_point(ui, ev->x, ev->y, &temp, &duty);

    size_t idx = (size_t)ui->drag_index;
    if (idx >= curve->npoints) {
        return FALSE;
    }

    /* Endpoints are pinned in temperature; the interior is monotonic. */
    if (idx == 0) {
        temp = curve->points[0].temp;
    } else if (idx + 1 == curve->npoints) {
        temp = curve->points[idx].temp;
    } else if (temp < curve->points[idx - 1].temp) {
        temp = curve->points[idx - 1].temp;
    }

    /* Propagate upward to the right, matching the previous editor. */
    curve->points[idx].temp = temp;
    curve->points[idx].duty = duty;
    for (size_t j = idx + 1; j < curve->npoints; j++) {
        if (curve->points[j].duty < curve->points[j - 1].duty) {
            curve->points[j].duty = curve->points[j - 1].duty;
        } else {
            break;
        }
    }
    for (size_t j = idx; j > 0; j--) {
        if (curve->points[j - 1].duty > curve->points[j].duty) {
            curve->points[j - 1].duty = curve->points[j].duty;
        } else {
            break;
        }
    }

    curve->samples_valid = FALSE;
    ui->dirty = TRUE;
    refresh_curve(ui);
    return TRUE;
}

/* --------------------------------------------------------------- banners */

static void show_banner(lg_ui *ui, const char *text, gboolean critical)
{
    if (text == NULL || text[0] == '\0') {
        gtk_widget_hide(ui->banner);
        return;
    }
    gtk_label_set_text(GTK_LABEL(ui->banner_label), text);
    gtk_widget_set_name(ui->banner, critical ? "banner-critical" : "banner-warn");
    gtk_widget_show(ui->banner);
}

/* ------------------------------------------------------------- callbacks */

static void on_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    UNUSED(sel);
    lg_ui *ui = data;
    lg_control *c = selected_control(ui);
    if (c != NULL) {
        snprintf(ui->cfg.config.selected_key, sizeof(ui->cfg.config.selected_key), "%s", c->key);
    }
    refresh_curve(ui);
}

static void on_apply_selected(GtkWidget *w, gpointer data)
{
    UNUSED(w);
    lg_ui *ui = data;
    lg_control *c = selected_control(ui);
    if (c == NULL) {
        set_status(ui, "No control is selected.");
        return;
    }
    if (!lg_config_curve(&ui->cfg.config, c)->enabled) {
        set_status(ui, "The curve for %s is disabled.", c->label);
        return;
    }
    request_apply(ui);
}

static void on_apply_all(GtkWidget *w, gpointer data)
{
    UNUSED(w);
    lg_ui *ui = data;
    request_apply(ui);
}

static void on_reset_curve(GtkWidget *w, gpointer data)
{
    UNUSED(w);
    lg_ui *ui = data;
    lg_control *c = selected_control(ui);
    if (c == NULL) {
        return;
    }
    lg_config_reset(&ui->cfg.config, c);
    lg_config_save(&ui->cfg.config);
    rebuild_controls(ui);
    refresh_curve(ui);
    set_status(ui, "Reset %s to its default curve.", c->label);
}

static void on_toggle_enabled(GtkWidget *w, gpointer data)
{
    lg_ui *ui = data;
    lg_control *c = selected_control(ui);
    if (c == NULL) {
        return;
    }
    lg_curve *curve = (lg_curve *)lg_config_curve(&ui->cfg.config, c);
    if (curve == NULL) {
        return;
    }
    curve->enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    lg_config_save(&ui->cfg.config);
    rebuild_controls(ui);
}

static void on_auto_toggled(GtkWidget *w, gpointer data)
{
    lg_ui *ui = data;
    ui->cfg.config.auto_apply = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    lg_config_save(&ui->cfg.config);
    set_status(ui, "Automatic curve application %s.",
               ui->cfg.config.auto_apply ? "enabled" : "disabled");
}

static void on_pause_toggled(GtkWidget *w, gpointer data)
{
    lg_ui *ui = data;
    ui->cfg.config.paused = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    lg_config_save(&ui->cfg.config);
    set_status(ui, ui->cfg.config.paused ? "Paused: readings continue, no writes are made."
                                         : "Resumed.");
}

static void on_floor_changed(GtkWidget *w, gpointer data)
{
    lg_ui *ui = data;
    lg_control *c = selected_control(ui);
    if (c == NULL) {
        return;
    }
    lg_curve *curve = (lg_curve *)lg_config_curve(&ui->cfg.config, c);
    if (curve == NULL) {
        return;
    }
    curve->min_duty = atoi(gtk_entry_get_text(GTK_ENTRY(w)));
    if (curve->min_duty < 0) {
        curve->min_duty = 0;
    }
    if (curve->min_duty > 100) {
        curve->min_duty = 100;
    }
    lg_config_save(&ui->cfg.config);
    refresh_curve(ui);
}

static void on_preset(GtkWidget *w, gpointer data)
{
    lg_ui *ui = data;
    const char *name = g_object_get_data(G_OBJECT(w), "preset");
    if (name == NULL) {
        return;
    }
    if (lg_config_apply_preset(&ui->cfg.config, name)) {
        lg_config_save(&ui->cfg.config);
        refresh_curve(ui);
        set_status(ui, "Applied the %s preset to every control.", name);
    }
}

static void on_theme(GtkWidget *w, gpointer data)
{
    UNUSED(w);
    lg_ui *ui = data;
    ui->dark_mode = !ui->dark_mode;
    apply_theme(ui);
    /* The curve editor paints its own background, so it needs to know too. */
    gtk_widget_queue_draw(ui->curve_area);
    ui->cfg.config.theme_light = ui->dark_mode ? false : true;
    lg_config_save(&ui->cfg.config);
    set_status(ui, "%s theme.", ui->dark_mode ? "Dark" : "Light");
}

static void on_failsafe(GtkWidget *w, gpointer data)
{
    lg_ui *ui = data;
    ui->cfg.config.failsafe_temp = atoi(gtk_entry_get_text(GTK_ENTRY(w)));
    lg_config_save(&ui->cfg.config);
    set_status(ui, "Failsafe threshold set to %d C.", ui->cfg.config.failsafe_temp);
}

/* ------------------------------------------------------------- exit path */

/*
 * Restore cooling to a safe state. Configurable because "full speed" and
 * "hand back to the board's own curve" are both defensible defaults.
 */
void lg_ui_restore_on_exit(lg_ui *ui)
{
    if (ui == NULL) {
        return;
    }
    if (ui->cfg.config.exit_mode == LG_EXIT_LEAVE) {
        return;
    }

    char err[192];
    for (size_t i = 0; i < ui->cfg.snapshot.ncontrols; i++) {
        lg_control *c = &ui->cfg.snapshot.controls[i];
        if (!lg_config_curve(&ui->cfg.config, c)->enabled) {
            continue;
        }
        if (ui->cfg.config.exit_mode == LG_EXIT_RESTORE_SPEED) {
            lg_control_full_speed(&ui->priv, c, err, sizeof(err));
        } else {
            lg_control_handback(&ui->priv, c, err, sizeof(err));
        }
    }
}

/* Stop the worker and make sure it cannot write again. */
static void stop_worker(lg_ui *ui)
{
    g_mutex_lock(&ui->worker_lock);
    ui->worker_quit = TRUE;
    g_cond_broadcast(&ui->worker_cond);
    g_mutex_unlock(&ui->worker_lock);

    if (ui->worker != NULL) {
        g_thread_join(ui->worker);
        ui->worker = NULL;
    }
}

void lg_ui_shutdown(lg_ui *ui)
{
    if (ui == NULL) {
        return;
    }
    /*
     * Order matters: the worker holds the only writer path, so it has to be
     * stopped before the restore runs, or it can re-apply a curve behind us.
     */
    stop_worker(ui);

    g_mutex_lock(&ui->config_lock);
    lg_ui_restore_on_exit(ui);
    lg_config_save(&ui->cfg.config);
    g_mutex_unlock(&ui->config_lock);

    gtk_main_quit();
}

static void on_window_destroy(GtkWidget *w, gpointer data)
{
    UNUSED(w);
    lg_ui_shutdown(data);
}

/* ----------------------------------------------------------------- worker */

/*
 * All sysfs reads and every privileged write happen here. The previous Tk
 * version ran this on the GUI thread, so the window froze for the duration of
 * each liquidctl invocation.
 */
static gpointer ui_worker_entry(gpointer data)
{
    lg_ui *ui = data;
    lg_discover_opts opts;
    lg_discover_opts_init(&opts);

    for (;;) {
        g_mutex_lock(&ui->worker_lock);
        while (!ui->worker_requested && !ui->worker_quit) {
            g_cond_wait(&ui->worker_cond, &ui->worker_lock);
        }
        if (ui->worker_quit) {
            g_mutex_unlock(&ui->worker_lock);
            break;
        }
        ui->worker_requested = FALSE;
        g_mutex_unlock(&ui->worker_lock);

        /*
         * lg_snapshot is around 580 KB because it holds every sensor the kernel
         * exposes. Holding that on a thread stack is what previously caused the
         * worker to overrun its stack and fault, so it lives on the heap.
         */
        lg_snapshot *snap = malloc(sizeof(*snap));
        if (snap == NULL) {
            break;
        }
        lg_discover(snap, &opts);

        /*
         * Carry per-channel knowledge across refreshes, matched on the stable
         * key. Without this every pass would forget which channels ignore
         * writes and retry them from scratch three seconds later.
         */
        const lg_snapshot *prev = &ui->cfg.snapshot;
        if (prev->ncontrols > 0) {
            for (size_t i = 0; i < snap->ncontrols; i++) {
                for (size_t j = 0; j < prev->ncontrols; j++) {
                    if (strcmp(prev->controls[j].key, snap->controls[i].key) != 0) {
                        continue;
                    }
                    if (snap->controls[i].enable_initial == -2 ||
                        snap->controls[i].enable_initial < 0) {
                        snap->controls[i].enable_initial = prev->controls[j].enable_initial;
                    }
                    /* A channel confirmed broken stays known-broken. */
                    if (prev->controls[j].responds == 0) {
                        snap->controls[i].responds = 0;
                    }
                    break;
                }
            }
        }

        int applied = 0;
        int failed = 0;
        int unresponsive = 0;
        int stalled = 0;
        bool failsafe = false;
        char message[256] = {0};

        bool do_apply = ui->cfg.config.auto_apply && !ui->cfg.config.paused;

        /* Failsafe overrides everything: any enabled control goes to full. */
        if (ui->cfg.config.failsafe_temp > 0 && !isnan(snap->cpu_temp) &&
            snap->cpu_temp >= (double)ui->cfg.config.failsafe_temp) {
            failsafe = true;
            do_apply = true;
        }

        /*
         * Copy the curve parameters out from under the config lock, then do
         * the (slow, privilege-crossing) writes without holding it.
         */
        typedef struct {
            bool enabled;
            int min_duty;
            lg_point points[LG_CURVE_MAX_POINTS];
            size_t npoints;
        } lg_curve_params;

        lg_curve_params params[LG_MAX_CONTROLS];
        size_t nparams = 0;

        g_mutex_lock(&ui->config_lock);
        for (size_t i = 0; i < snap->ncontrols; i++) {
            const lg_curve *curve = lg_config_curve(&ui->cfg.config, &snap->controls[i]);
            if (curve == NULL || nparams >= LG_MAX_CONTROLS) {
                continue;
            }
            params[nparams].enabled = curve->enabled;
            params[nparams].min_duty = curve->min_duty;
            params[nparams].npoints = curve->npoints;
            memcpy(params[nparams].points, curve->points,
                   curve->npoints * sizeof(lg_point));
            nparams++;
        }
        g_mutex_unlock(&ui->config_lock);

        if (do_apply) {
            for (size_t i = 0; i < snap->ncontrols && i < nparams; i++) {
                lg_control *c = &snap->controls[i];
                const lg_curve_params *p = &params[i];

                int duty;
                if (failsafe) {
                    duty = 100;
                } else {
                    if (!p->enabled) {
                        continue;
                    }
                    if (isnan(snap->cpu_temp)) {
                        continue;
                    }
                    duty = lg_curve_duty_points(p->points, p->npoints, snap->cpu_temp);
                    if (p->min_duty > duty) {
                        duty = p->min_duty;
                    }
                }

                char err[192] = {0};

                /*
                 * A channel already known to ignore writes gets a single try.
                 * Repeating four times on every cycle would add seconds of
                 * latency, including to shutdown, for a result already known.
                 * The knowledge survives refreshes because it is carried across
                 * by key after discovery.
                 */
                const int attempts = (c->responds == 0) ? 1 : 4;
                lg_apply_result r =
                    lg_control_apply_verified_n(&ui->priv, c, duty, attempts, err, sizeof(err));
                if (r == LG_APPLY_OK) {
                    applied++;
                    c->duty = duty;
                    c->responds = 1;
                } else if (r == LG_APPLY_UNRESPONSIVE) {
                    /* The driver accepted the write and discarded it. Report
                     * it rather than letting the user believe the channel is
                     * under control. */
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
        }

        /* Refresh the readback and look for stalled fans. */
        for (size_t i = 0; i < snap->ncontrols; i++) {
            lg_control *c = &snap->controls[i];
            lg_control_refresh(c);

            bool spinning_requested = (c->duty >= ui->cfg.config.stall_duty) &&
                                     ui->cfg.config.stall_duty > 0 && c->has_fan_pair;
            if (spinning_requested && c->fan_rpm == 0) {
                if (ui->stall_count[i] < ui->cfg.config.stall_samples) {
                    ui->stall_count[i]++;
                }
                if (ui->stall_count[i] >= ui->cfg.config.stall_samples) {
                    stalled++;
                }
            } else {
                ui->stall_count[i] = 0;
            }
        }

        g_mutex_lock(&ui->worker_lock);
        ui->pending = *snap;
        ui->pending_valid = TRUE;
        ui->pending_applied = applied;
        ui->pending_failed = failed;
        ui->pending_unresponsive = unresponsive;
        ui->pending_stalled = stalled;
        ui->pending_failsafe = failsafe;
        ui->pending_failsafe_temp = snap->cpu_temp;
        snprintf(ui->pending_message, sizeof(ui->pending_message), "%s", message);
        g_mutex_unlock(&ui->worker_lock);

        free(snap);

        /* Loop back and wait for the next request. */
    }

    return NULL;
}

/* ------------------------------------------------------------------ build */

static void apply_theme(lg_ui *ui)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        provider,
        lg_theme_css(ui->dark_mode ? LG_THEME_MODE_DARK : LG_THEME_MODE_LIGHT), -1, NULL);

    GdkScreen *screen = gdk_screen_get_default();

    /*
     * Drop the previous provider first. Adding one per toggle without removing
     * the last stacks providers at the same priority, so a rule that exists in
     * only one palette keeps leaking into the other and the result depends on
     * which theme was applied last.
     */
    if (ui->css_provider != NULL) {
        gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(ui->css_provider));
        g_object_unref(ui->css_provider);
    }

    /*
     * The base theme's dark/light mode is NOT switched here. Toggling
     * gtk-application-prefer-dark-theme at runtime forces GTK to reload the
     * theme, which tears down and rebuilds each TreeView's header widget, and
     * the headers do not come back. The palette below therefore sets every
     * surface it cares about explicitly, including the header and its buttons,
     * rather than relying on the theme to be in a particular mode.
     *
     * main.c sets the preference once, before any widget is built, so the
     * theme loads in the right mode from the start.
     */
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    ui->css_provider = provider;

    gtk_widget_queue_draw(ui->window);
    if (ui->controls_view != NULL) {
        gtk_widget_queue_draw(ui->controls_view);
    }
    if (ui->curve_area != NULL) {
        gtk_widget_queue_draw(ui->curve_area);
    }
}

GtkWidget *lg_ui_new(lg_ui_config *cfg)
{
    lg_ui *ui = g_new0(lg_ui, 1);
    ui->cfg = *cfg;
    ui->drag_index = -1;
    ui->dark_mode = (cfg->theme != LG_THEME_LIGHT);
    g_mutex_init(&ui->config_lock);
    g_mutex_init(&ui->worker_lock);
    g_cond_init(&ui->worker_cond);

    lg_priv_detect(&ui->priv);

    ui->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui->window), "LiquidGUI");
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 1180, 780);
    gtk_window_set_titlebar(GTK_WINDOW(ui->window), NULL);
    g_signal_connect(ui->window, "destroy", G_CALLBACK(on_window_destroy), ui);

    apply_theme(ui);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(ui->window), root);

    /* ---- header ---- */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_end(header, 12);
    gtk_widget_set_margin_top(header, 10);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

    ui->header_temp = gtk_label_new("CPU package: --");
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.6));
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(ui->header_temp), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(header), ui->header_temp, FALSE, FALSE, 0);

    ui->header_coolant = gtk_label_new("Coolant: --");
    gtk_box_pack_start(GTK_BOX(header), ui->header_coolant, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(header), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    ui->pause_check = gtk_check_button_new_with_label("Pause writes");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->pause_check), ui->cfg.config.paused);
    g_signal_connect(ui->pause_check, "toggled", G_CALLBACK(on_pause_toggled), ui);
    gtk_box_pack_start(GTK_BOX(header), ui->pause_check, FALSE, FALSE, 0);

    ui->auto_check = gtk_check_button_new_with_label("Auto apply");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->auto_check), ui->cfg.config.auto_apply);
    g_signal_connect(ui->auto_check, "toggled", G_CALLBACK(on_auto_toggled), ui);
    gtk_box_pack_start(GTK_BOX(header), ui->auto_check, FALSE, FALSE, 0);

    /* Preset menu. */
    GtkWidget *preset_btn = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(preset_btn), "Presets");
    GtkWidget *menu = gtk_menu_new();
    for (const char *const *n = lg_config_preset_names(); *n != NULL; n++) {
        GtkWidget *item = gtk_menu_item_new_with_label(*n);
        g_object_set_data(G_OBJECT(item), "preset", (gpointer)*n);
        g_signal_connect(item, "activate", G_CALLBACK(on_preset), ui);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(preset_btn), menu);
    gtk_box_pack_start(GTK_BOX(header), preset_btn, FALSE, FALSE, 0);

    GtkWidget *theme_btn = gtk_button_new_with_label("Theme");
    g_signal_connect(theme_btn, "clicked", G_CALLBACK(on_theme), ui);
    gtk_box_pack_start(GTK_BOX(header), theme_btn, FALSE, FALSE, 0);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(header), spacer, TRUE, TRUE, 0);

    ui->priv_label = gtk_label_new(lg_priv_describe(&ui->priv));
    gtk_box_pack_start(GTK_BOX(header), ui->priv_label, FALSE, FALSE, 0);

    /* ---- banner ---- */
    ui->banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui->banner_label = gtk_label_new("");
    gtk_widget_set_margin_start(ui->banner_label, 10);
    gtk_box_pack_start(GTK_BOX(ui->banner), ui->banner_label, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(root), ui->banner, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(ui->banner, TRUE);
    gtk_widget_hide(ui->banner);

    /* ---- main panes ---- */
    GtkWidget *panes = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(panes, TRUE);
    gtk_box_pack_start(GTK_BOX(root), panes, TRUE, TRUE, 0);

    /* Left: controls. */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(left, 10);
    gtk_widget_set_margin_top(left, 8);
    gtk_widget_set_margin_bottom(left, 8);
    gtk_paned_pack1(GTK_PANED(panes), left, FALSE, FALSE);

    ui->controls_store = gtk_list_store_new(9,
        G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    ui->controls_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->controls_store));

    /* Column 0 is the control index and column 9 is the storage key; both are
     * used for lookup and neither is shown. */
    static const char *const ctrl_titles[] = {
        "",  "Control", "Device", "Now", "Curve", "RPM", "Mode", "On", "Key",
    };
    const int ctrl_columns = (int)(sizeof(ctrl_titles) / sizeof(ctrl_titles[0]));
    for (int i = 0; i < ctrl_columns; i++) {
        GtkCellRenderer *rend = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col =
            gtk_tree_view_column_new_with_attributes(ctrl_titles[i], rend, "text", i, NULL);
        if (i == 0 || i == ctrl_columns - 1) {
            gtk_tree_view_column_set_visible(col, FALSE);
        }
        gtk_tree_view_append_column(GTK_TREE_VIEW(ui->controls_view), col);
    }

    GtkWidget *ctrl_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ctrl_scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ctrl_scroll), ui->controls_view);
    gtk_box_pack_start(GTK_BOX(left), ctrl_scroll, TRUE, TRUE, 0);

    GtkTreeSelection *ctrl_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->controls_view));
    gtk_tree_selection_set_mode(ctrl_sel, GTK_SELECTION_SINGLE);
    g_signal_connect(ctrl_sel, "changed", G_CALLBACK(on_selection_changed), ui);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_apply = gtk_button_new_with_label("Apply selected");
    g_signal_connect(b_apply, "clicked", G_CALLBACK(on_apply_selected), ui);
    GtkWidget *b_apply_all = gtk_button_new_with_label("Apply all");
    g_signal_connect(b_apply_all, "clicked", G_CALLBACK(on_apply_all), ui);
    gtk_box_pack_start(GTK_BOX(btns), b_apply, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btns), b_apply_all, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(left), btns, FALSE, FALSE, 0);

    GtkWidget *b_reset = gtk_button_new_with_label("Reset curve");
    g_signal_connect(b_reset, "clicked", G_CALLBACK(on_reset_curve), ui);
    gtk_box_pack_start(GTK_BOX(left), b_reset, FALSE, FALSE, 0);

    GtkWidget *enabled_check = gtk_check_button_new_with_label("Curve enabled");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled_check), TRUE);
    g_signal_connect(enabled_check, "toggled", G_CALLBACK(on_toggle_enabled), ui);
    gtk_box_pack_start(GTK_BOX(left), enabled_check, FALSE, FALSE, 0);

    GtkWidget *floor_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *floor_label = gtk_label_new("Minimum duty");
    GtkWidget *floor_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(floor_entry), 4);
    gtk_entry_set_text(GTK_ENTRY(floor_entry), "0");
    g_signal_connect(floor_entry, "changed", G_CALLBACK(on_floor_changed), ui);
    gtk_box_pack_start(GTK_BOX(floor_box), floor_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(floor_box), floor_entry, FALSE, FALSE, 0);
    floor_box = floor_box;
    gtk_box_pack_start(GTK_BOX(left), floor_box, FALSE, FALSE, 0);

    GtkWidget *fs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *fs_label = gtk_label_new("Failsafe above");
    GtkWidget *fs_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(fs_entry), 4);
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", ui->cfg.config.failsafe_temp);
        gtk_entry_set_text(GTK_ENTRY(fs_entry), buf);
    }
    g_signal_connect(fs_entry, "changed", G_CALLBACK(on_failsafe), ui);
    gtk_box_pack_start(GTK_BOX(fs_box), fs_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fs_box), fs_entry, FALSE, FALSE, 0);
    fs_box = fs_box;
    gtk_box_pack_start(GTK_BOX(left), fs_box, FALSE, FALSE, 0);

    /* Right: curve editor over a sensor notebook. */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(right, 10);
    gtk_widget_set_margin_end(right, 10);
    gtk_widget_set_margin_top(right, 8);
    gtk_paned_pack2(GTK_PANED(panes), right, TRUE, TRUE);

    ui->curve_info = gtk_label_new("Select a control to edit its curve.");
    gtk_label_set_xalign(GTK_LABEL(ui->curve_info), 0.0);
    gtk_box_pack_start(GTK_BOX(right), ui->curve_info, FALSE, FALSE, 0);

    ui->curve_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(ui->curve_area, 320, 280);
    gtk_widget_add_events(ui->curve_area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                            GDK_POINTER_MOTION_MASK);
    gtk_widget_set_can_focus(ui->curve_area, TRUE);
    g_signal_connect(ui->curve_area, "draw", G_CALLBACK(curve_draw_cb), ui);
    g_signal_connect(ui->curve_area, "button-press-event", G_CALLBACK(curve_button_cb), ui);
    g_signal_connect(ui->curve_area, "button-release-event", G_CALLBACK(curve_button_cb), ui);
    g_signal_connect(ui->curve_area, "motion-notify-event", G_CALLBACK(curve_motion_cb), ui);
    gtk_box_pack_start(GTK_BOX(right), ui->curve_area, TRUE, TRUE, 0);

    ui->detail_label = gtk_label_new(
        "Drag a point to reshape the curve. Right click a point to remove it. "
        "Raising a point lifts the points to its right; the minimum duty floor is "
        "applied after the curve is evaluated.");
    gtk_label_set_xalign(GTK_LABEL(ui->detail_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(ui->detail_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui->detail_label), 80);
    gtk_box_pack_start(GTK_BOX(right), ui->detail_label, FALSE, FALSE, 0);

    ui->notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(ui->notebook, TRUE);
    gtk_box_pack_start(GTK_BOX(right), ui->notebook, TRUE, TRUE, 0);

    for (int cls = 0; cls < LG_SENSOR_CLASS_COUNT; cls++) {
        ui->sensor_stores[cls] = gtk_list_store_new(7,
            G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
            G_TYPE_STRING, G_TYPE_INT);
        GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->sensor_stores[cls]));
        GtkCellRenderer *rend = gtk_cell_renderer_text_new();

        const char *titles[] = {"Sensor", "Device", "Driver", "Reading", "Range", "Note", ""};
        for (int i = 0; i < 6; i++) {
            GtkTreeViewColumn *col =
                gtk_tree_view_column_new_with_attributes(titles[i], rend, "text", i, NULL);
            gtk_tree_view_column_set_resizable(col, TRUE);
            gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
        }

        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                       GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), view);
        ui->sensor_views[cls] = view;

        gtk_notebook_append_page(GTK_NOTEBOOK(ui->notebook), scroll,
                                 gtk_label_new(lg_sensor_class_name((lg_sensor_class)cls)));
    }

    ui->status_label = gtk_label_new("Starting up.");
    gtk_label_set_xalign(GTK_LABEL(ui->status_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(ui->status_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(ui->status_label, 12);
    gtk_widget_set_margin_end(ui->status_label, 12);
    gtk_widget_set_margin_top(ui->status_label, 6);
    gtk_widget_set_margin_bottom(ui->status_label, 6);
    gtk_box_pack_start(GTK_BOX(root), ui->status_label, FALSE, FALSE, 0);

    gtk_paned_set_position(GTK_PANED(panes), 420);

    /* Accelerators. */
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(ui->window), accel);
    gtk_widget_add_accelerator(b_apply, "clicked", accel, GDK_KEY_a, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(b_apply_all, "clicked", accel, GDK_KEY_A, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(b_reset, "clicked", accel, GDK_KEY_r, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(ui->pause_check, "clicked", accel, GDK_KEY_space, 0,
                               GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(theme_btn, "clicked", accel, GDK_KEY_t, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);

    rebuild_controls(ui);
    rebuild_sensors(ui);

    /* Land on a control immediately rather than showing an empty editor. */
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->controls_store), &first)) {
        gtk_tree_selection_select_iter(ctrl_sel, &first);
    }
    refresh_curve(ui);

    ui->worker = g_thread_new("liquidgui-io", ui_worker_entry, ui);
    request_refresh(ui);

    g_object_set_data_full(G_OBJECT(ui->window), "lg-ui", ui, g_free);
    return ui->window;
}

/*
 * Called on the GTK main thread once the worker has published a snapshot.
 * Kept in the header so the timer callback in main.c can drive it.
 */
void lg_ui_poll(lg_ui *ui)
{
    /* Heap, for the same reason as the worker's copy: see ui_worker_entry. */
    lg_snapshot *snap = malloc(sizeof(*snap));
    char message[256];
    int applied, failed, unresponsive, stalled;
    gboolean failsafe;
    double failsafe_temp;

    if (snap == NULL) {
        return;
    }

    g_mutex_lock(&ui->worker_lock);
    if (!ui->pending_valid) {
        g_mutex_unlock(&ui->worker_lock);
        free(snap);
        return;
    }
    *snap = ui->pending;
    snprintf(message, sizeof(message), "%s", ui->pending_message);
    applied = ui->pending_applied;
    failed = ui->pending_failed;
    unresponsive = ui->pending_unresponsive;
    stalled = ui->pending_stalled;
    failsafe = ui->pending_failsafe;
    failsafe_temp = ui->pending_failsafe_temp;
    ui->pending_valid = FALSE;
    g_mutex_unlock(&ui->worker_lock);

    ui->cfg.snapshot = *snap;
    free(snap);

    rebuild_controls(ui);
    rebuild_sensors(ui);

    if (isnan(ui->cfg.snapshot.cpu_temp)) {
        gtk_label_set_text(GTK_LABEL(ui->header_temp), "CPU package: n/a");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "CPU package: %.1f C", ui->cfg.snapshot.cpu_temp);
        gtk_label_set_text(GTK_LABEL(ui->header_temp), buf);
    }

    if (isnan(ui->cfg.snapshot.coolant_temp)) {
        gtk_label_set_text(GTK_LABEL(ui->header_coolant), "Coolant: n/a");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Coolant: %.1f C", ui->cfg.snapshot.coolant_temp);
        gtk_label_set_text(GTK_LABEL(ui->header_coolant), buf);
    }

    refresh_curve(ui);

    if (failsafe) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Thermal failsafe: %.1f C is at or above the %d C threshold. "
                 "Every enabled control is being held at full speed.",
                 failsafe_temp, ui->cfg.config.failsafe_temp);
        show_banner(ui, buf, TRUE);
    } else if (stalled > 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%d fan(s) are commanded to at least %d%% but report 0 rpm. "
                 "Check for a stalled or disconnected fan.", stalled,
                 ui->cfg.config.stall_duty);
        show_banner(ui, buf, TRUE);
    } else if (unresponsive > 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "%d channel(s) accepted a write but did not change: %s",
                 unresponsive, message);
        show_banner(ui, buf, TRUE);
    } else if (failed > 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%d control(s) could not be written: %s", failed, message);
        show_banner(ui, buf, FALSE);
    } else {
        show_banner(ui, "", FALSE);
    }

    if (ui->cfg.config.paused) {
        set_status(ui, "Paused. %zu sensors, %zu controls, no writes.",
                   ui->cfg.snapshot.nsensors, ui->cfg.snapshot.ncontrols);
    } else if (ui->cfg.config.auto_apply) {
        set_status(ui, "Applied %d curve(s); %zu sensors, %zu controls.", applied,
                   ui->cfg.snapshot.nsensors, ui->cfg.snapshot.ncontrols);
    } else {
        set_status(ui, "%zu sensors, %zu controls. Automatic application is off.",
                   ui->cfg.snapshot.nsensors, ui->cfg.snapshot.ncontrols);
    }
}

void lg_ui_request(lg_ui *ui)
{
    request_refresh(ui);
}

/* Pending screenshot request, serviced by the main loop. */
static char *g_shot_path = NULL;

/* A compositor can hand back a uniformly black buffer; treat that as no data. */
static gboolean pixbuf_has_content(GdkPixbuf *pb)
{
    if (pb == NULL) {
        return FALSE;
    }
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    if (w <= 0 || h <= 0) {
        return FALSE;
    }

    /* Sample a sparse grid and require more than one distinct colour. */
    guint32 first = 0;
    int distinct = 0;
    for (int y = 0; y < h; y += (h / 24 > 0 ? h / 24 : 1)) {
        for (int x = 0; x < w; x += (w / 24 > 0 ? w / 24 : 1)) {
            guchar *p = gdk_pixbuf_get_pixels(pb) + (y * gdk_pixbuf_get_rowstride(pb)) + x * nch;
            guint32 v = 0;
            for (int c = 0; c < nch && c < 3; c++) {
                v = (v << 8) | p[c];
            }
            if (distinct == 0) {
                first = v;
                distinct = 1;
            } else if (v != first && distinct < 2) {
                distinct = 2;
            }
        }
    }
    return distinct >= 2;
}

static gboolean shoot_cb(gpointer data)
{
    lg_ui *ui = data;
    if (g_shot_path == NULL) {
        return G_SOURCE_REMOVE;
    }

    /*
     * Wait for the first worker pass to publish rather than guessing at a
     * delay: the first pass also applies curves, and a couple of unresponsive
     * headers mean it can take several seconds. Capturing early yields a
     * half-populated window.
     */
    gboolean have_data = FALSE;
    g_mutex_lock(&ui->worker_lock);
    have_data = ui->pending_valid;
    g_mutex_unlock(&ui->worker_lock);
    if (!have_data) {
        return G_SOURCE_CONTINUE;
    }

    /*
     * Adopt the snapshot first: lg_ui_poll is what fills the header and the
     * status bar, so capturing without it would show an empty frame even
     * though the worker has data ready.
     */
    lg_ui_poll(ui);

    /* Drain pending work so the views reflect the latest snapshot. */
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }
    gtk_widget_queue_draw(ui->window);
    gtk_widget_queue_draw(ui->curve_area);
    while (gtk_events_pending()) {
        gtk_main_iteration_do(FALSE);
    }

    /*
     * Render the widget tree into an offscreen surface. A screen grab depends
     * on the compositor having the current frame and can return an empty image,
     * so the offscreen pass is the reliable fallback rather than the only
     * option.
     *
     * It is not, however, complete: gtk_widget_draw does not render a
     * TreeView's column headers, so an offscreen capture shows the rows with
     * no headings at all. That is misleading when the point of the capture is
     * to check appearance, so prefer a real read of the window and fall back
     * to the offscreen render when the compositor gives us nothing back.
     */
    GtkAllocation alloc;
    gtk_widget_get_allocation(ui->window, &alloc);
    if (alloc.width <= 0 || alloc.height <= 0) {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, alloc.width, alloc.height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }

    cairo_t *cr = cairo_create(surf);
    gtk_widget_draw(ui->window, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    cairo_status_t st = cairo_surface_write_to_png(surf, g_shot_path);
    cairo_surface_destroy(surf);

    /* Try the real window; it is the only capture that includes headers. */
    gboolean real_ok = FALSE;
    if (gtk_widget_get_window(ui->window) != NULL) {
        GdkPixbuf *shot = gdk_pixbuf_get_from_window(gtk_widget_get_window(ui->window), 0, 0,
                                                        alloc.width, alloc.height);
        if (shot != NULL) {
            real_ok = pixbuf_has_content(shot);
            if (real_ok) {
                GError *e = NULL;
                if (!gdk_pixbuf_save(shot, g_shot_path, "png", &e, NULL)) {
                    real_ok = FALSE;
                    g_clear_error(&e);
                }
            }
            g_object_unref(shot);
        }
    }

    if (real_ok) {
        g_print("screenshot written to %s\n", g_shot_path);
    } else if (st == CAIRO_STATUS_SUCCESS) {
        g_print("screenshot written to %s (offscreen: headers not captured)\n", g_shot_path);
    } else {
        g_printerr("screenshot failed: %s\n", cairo_status_to_string(st));
    }

    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

void lg_ui_screenshot_and_quit(lg_ui *ui, const char *path)
{
    if (ui == NULL || path == NULL) {
        return;
    }
    g_free(g_shot_path);
    g_shot_path = g_strdup(path);
    /* Give discovery, the curve editor and the sensor tree time to populate. */
    /* Poll for readiness; the callback removes itself once it fires. */
    g_timeout_add(250, shoot_cb, ui);
}

lg_ui_config *lg_ui_config_of(lg_ui *ui)
{
    return (ui != NULL) ? &ui->cfg : NULL;
}
