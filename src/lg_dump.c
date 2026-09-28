/*
 * lg_dump - render a discovery snapshot as JSON.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include "lg_dump.h"

#include <stdio.h>
#include <string.h>

#include "lg_hwmon.h"
#include "lg_json.h"

/*
 * Tiny helper that remembers whether a separator is needed, so commas cannot
 * be forgotten between members or elements.
 */
typedef struct {
    lg_json_writer *w;
    int depth;
    bool need_comma;
} lg_emit;

static void emit_indent(lg_emit *e)
{
    lg_json_raw(e->w, "\n");
    for (int i = 0; i < e->depth; i++) {
        lg_json_raw(e->w, "  ");
    }
}

static void emit_sep(lg_emit *e)
{
    if (e->need_comma) {
        lg_json_raw(e->w, ",");
    }
    emit_indent(e);
    e->need_comma = true;
}

char *lg_snapshot_to_json(const lg_snapshot *snap, int indent_depth)
{
    (void)indent_depth;
    if (snap == NULL) {
        return NULL;
    }

    lg_json_writer w;
    lg_json_writer_init(&w);
    lg_emit e = {.w = &w, .depth = 0, .need_comma = false};

    lg_json_begin_object(&w);

    /* --- controls --- */
    emit_sep(&e);
    lg_json_key(&w, "controls");
    lg_json_begin_array(&w);
    for (size_t i = 0; i < snap->ncontrols; i++) {
        const lg_control *c = &snap->controls[i];
        if (i > 0) {
            lg_json_raw(&w, ",");
        }
        emit_indent(&e);
        e.depth = 2;

        lg_json_begin_object(&w);
        lg_json_key(&w, "key");
        lg_json_write_str(&w, c->key);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "chip");
        lg_json_write_str(&w, c->chip);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "driver");
        lg_json_write_str(&w, c->driver);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "label");
        lg_json_write_str(&w, c->label);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "kind");
        lg_json_write_str(&w, c->kind == LG_CTRL_LIQUIDCTL ? "liquidctl" : "hwmon");
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "channel");
        lg_json_write_num(&w, c->channel);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "duty");
        lg_json_write_num(&w, c->duty);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "pwm_path");
        lg_json_write_str(&w, c->pwm_path);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "needs_enable_first");
        lg_json_write_bool(&w, c->needs_enable_first);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "is_aio");
        lg_json_write_bool(&w, c->is_aio);
        lg_json_raw(&w, ", ");
        lg_json_key(&w, "fan_rpm");
        lg_json_write_num(&w, c->fan_rpm);
        lg_json_end_object(&w);
        e.depth = 0;
    }
    lg_json_end_array(&w);

    /* --- sensors, grouped by class --- */
    static const lg_sensor_class order[] = {
        LG_SENSOR_TEMP, LG_SENSOR_FAN, LG_SENSOR_VOLT, LG_SENSOR_CURRENT, LG_SENSOR_POWER,
    };
    for (size_t oi = 0; oi < sizeof(order) / sizeof(order[0]); oi++) {
        emit_sep(&e);
        char key[64];
        snprintf(key, sizeof(key), "%s_list", lg_sensor_class_name(order[oi]));
        lg_json_key(&w, key);
        lg_json_begin_array(&w);

        bool first = true;
        for (size_t i = 0; i < snap->nsensors; i++) {
            const lg_sensor *s = &snap->sensors[i];
            if (s->cls != order[oi]) {
                continue;
            }
            if (!first) {
                lg_json_raw(&w, ",");
            }
            emit_indent(&e);
            e.depth = 2;
            first = false;

            lg_json_begin_object(&w);
            lg_json_key(&w, "chip");
            lg_json_write_str(&w, s->chip);
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "driver");
            lg_json_write_str(&w, s->driver);
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "label");
            lg_json_write_str(&w, s->label);
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "index");
            lg_json_write_num(&w, s->index);
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "value");
            lg_json_write_num(&w, s->value);
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "valid");
            lg_json_write_bool(&w, s->valid);
            lg_json_raw(&w, ", ");
            /*
             * The strings the interface actually renders, not just the numbers
             * behind them. A display bug is invisible to a dump that only
             * carries values: a range printed as 2.82e+03 .. 4.1e+03 looked
             * perfectly correct in the underlying data, and the numbers alone
             * would not have shown it.
             */
            char rendered[64];
            char range[96];
            lg_sensor_format(s, rendered, sizeof(rendered));
            char lo[32];
            char hi[32];
            if (s->has_min && s->has_max) {
                lg_sensor_format_bound(s, s->min, lo, sizeof(lo));
                lg_sensor_format_bound(s, s->max, hi, sizeof(hi));
                snprintf(range, sizeof(range), "%s .. %s", lo, hi);
            } else if (s->has_crit) {
                lg_sensor_format_bound(s, s->crit, lo, sizeof(lo));
                snprintf(range, sizeof(range), "crit %s", lo);
            } else {
                snprintf(range, sizeof(range), "--");
            }
            lg_json_key(&w, "display");
            lg_json_write_str(&w, rendered);
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "range_display");
            lg_json_write_str(&w, range);
            lg_json_raw(&w, ", ");
            lg_json_key(&w, "note");
            lg_json_write_str(&w, s->note);
            lg_json_end_object(&w);
            e.depth = 0;
        }
        lg_json_end_array(&w);
    }

    /* --- derived state --- */
    emit_sep(&e);
    lg_json_key(&w, "cpu_temp_c");
    lg_json_write_num(&w, snap->cpu_temp);
    emit_sep(&e);
    lg_json_key(&w, "coolant_temp_c");
    lg_json_write_num(&w, snap->coolant_temp);
    emit_sep(&e);
    lg_json_key(&w, "nsensors");
    lg_json_write_num(&w, (double)snap->nsensors);
    emit_sep(&e);
    lg_json_key(&w, "ncontrols");
    lg_json_write_num(&w, (double)snap->ncontrols);
    emit_sep(&e);
    lg_json_key(&w, "notes");
    lg_json_write_str(&w, snap->notes);

    lg_json_end_object(&w);
    lg_json_raw(&w, "\n");

    return lg_json_writer_take(&w);
}
