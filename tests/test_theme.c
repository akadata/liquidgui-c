/*
 * Palette contrast tests.
 *
 * This exists because of a reported defect: in light mode the table headings
 * were unreadable, showing near-black text on the dark heading background. The
 * cause was that a TreeView column heading is a separate header strip of
 * buttons, so styling `treeview` left the headings on the theme's own colours
 * while the `label` rule recoloured their text.
 *
 * The palettes are plain data in src/lg_theme.c, so the heading foreground and
 * background can be checked here without a display, and the requirement is
 * stated rather than left to be spotted in a screenshot.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lg_theme.h"

static int failures = 0;
static long checks = 0;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* ------------------------------------------------------------------ colour */

typedef struct {
    double r, g, b;
} lg_rgb;

static int parse_hex(const char *hex, lg_rgb *out)
{
    unsigned int v = 0;
    if (sscanf(hex, "#%6x", &v) != 1) {
        return 0;
    }
    out->r = (double)((v >> 16) & 0xFF);
    out->g = (double)((v >> 8) & 0xFF);
    out->b = (double)(v & 0xFF);
    return 1;
}

static double channel_luminance(double v255)
{
    double v = v255 / 255.0;
    return (v <= 0.04045) ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double relative_luminance(lg_rgb c)
{
    return 0.2126 * channel_luminance(c.r) + 0.7152 * channel_luminance(c.g) +
           0.0722 * channel_luminance(c.b);
}

/* WCAG 2.1 contrast ratio. */
static double contrast_ratio(lg_rgb a, lg_rgb b)
{
    double la = relative_luminance(a);
    double lb = relative_luminance(b);
    double hi = (la > lb) ? la : lb;
    double lo = (la > lb) ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

/* -------------------------------------------------------------- css lookup */

/*
 * Find "background-color: #rrggbb" in the rule whose selector list contains
 * `selector`, stopping at the next rule so a later rule is not picked up.
 */
static bool css_color_for(const char *css, const char *selector, const char *property,
                          lg_rgb *out, char *hex_out, size_t hex_cap)
{
    size_t sel_len = strlen(selector);
    const char *p = css;

    while ((p = strstr(p, selector)) != NULL) {
        /* The selector must start this rule. */
        bool at_rule_start = (p == css) || p[-1] == '{' || p[-1] == '\n' ||
                              p[-1] == ' ' || p[-1] == ',';
        if (!at_rule_start) {
            p += sel_len;
            continue;
        }

        const char *brace = strchr(p, '{');
        if (brace == NULL) {
            return false;
        }
        const char *end = strchr(brace, '}');
        if (end == NULL) {
            return false;
        }

        char body[2048];
        size_t blen = (size_t)(end - brace - 1);
        if (blen >= sizeof(body)) {
            blen = sizeof(body) - 1;
        }
        memcpy(body, brace + 1, blen);
        body[blen] = '\0';

        /*
         * Match the property as a whole name. A plain substring search for
         * "color" would also match the tail of "background-color" in the same
         * rule and report the background as the foreground.
         */
        size_t plen = strlen(property);
        for (const char *q = body; *q != '\0'; q++) {
            if (strncmp(q, property, plen) != 0) {
                continue;
            }
            bool at_start = (q == body) || q[-1] == ';' || q[-1] == ' ';
            if (!at_start) {
                continue;
            }
            const char *colon = q + plen;
            while (*colon == ' ') {
                colon++;
            }
            if (*colon != ':') {
                continue;
            }

            const char *hash = strchr(colon, '#');
            if (hash != NULL && hash[7] != '\0') {
                char hex[8];
                memcpy(hex, hash, 7);
                hex[7] = '\0';
                if (parse_hex(hex, out)) {
                    if (hex_out != NULL && hex_cap > 0) {
                        snprintf(hex_out, hex_cap, "%s", hex);
                    }
                    return true;
                }
            }
        }
        p = end;
    }
    return false;
}

static bool css_has(const char *css, const char *needle)
{
    return strstr(css, needle) != NULL;
}

/* ------------------------------------------------------------------- tests */

static void test_header_rules_exist(lg_theme_mode mode, const char *name)
{
    const char *css = lg_theme_css(mode);

    check(css_has(css, "treeview header"),
          "palette styles the treeview header, not just the treeview");
    check(css_has(css, "treeview header button"),
          "palette styles the header buttons");
    check(css_has(css, "treeview header button label"),
          "palette styles the header label, which is where the text colour lives");
    check(css_has(css, "background-image: none"),
          "palette clears the theme's background image, which would hide the colour");
    check(css_has(css, "notebook > header > tabs > tab"),
          "palette styles the notebook tabs");

    /* The reported defect: heading text unreadable. */
    lg_rgb bg = {0, 0, 0};
    lg_rgb fg = {0, 0, 0};
    char bg_hex[8] = {0};
    char fg_hex[8] = {0};

    bool have_bg = css_color_for(css, "treeview header", "background-color", &bg, bg_hex,
                                 sizeof(bg_hex));
    bool have_fg = css_color_for(css, "treeview header button label", "color", &fg, fg_hex,
                                 sizeof(fg_hex));

    checks++;
    if (!have_bg || !have_fg) {
        printf("FAIL: %s palette is missing a header colour (bg=%d fg=%d)\n", name, have_bg,
               have_fg);
        failures++;
        return;
    }

    double ratio = contrast_ratio(fg, bg);
    printf("  %s heading: text %s on %s = %.2f:1\n", name, fg_hex, bg_hex, ratio);
    check(ratio >= 4.5, "heading text meets the 4.5:1 minimum contrast");
    check(ratio >= 7.0, "heading text reaches the 7:1 AAA contrast target");
}

static void test_body_contrast(lg_theme_mode mode, const char *name)
{
    const char *css = lg_theme_css(mode);

    lg_rgb bg = {0, 0, 0};
    lg_rgb fg = {0, 0, 0};
    char bg_hex[8] = {0};
    char fg_hex[8] = {0};

    if (!css_color_for(css, "treeview, treeview.view", "background-color", &bg, bg_hex,
                       sizeof(bg_hex)) ||
        !css_color_for(css, "treeview, treeview.view", "color", &fg, fg_hex, sizeof(fg_hex))) {
        printf("FAIL: %s palette has no treeview body colour\n", name);
        failures++;
        checks++;
        return;
    }

    double ratio = contrast_ratio(fg, bg);
    printf("  %s rows:     text %s on %s = %.2f:1\n", name, fg_hex, bg_hex, ratio);
    check(ratio >= 4.5, "row text meets the 4.5:1 minimum contrast");
}

static void test_palettes_differ(void)
{
    const char *dark = lg_theme_css(LG_THEME_MODE_DARK);
    const char *light = lg_theme_css(LG_THEME_MODE_LIGHT);
    check(strcmp(dark, light) != 0, "the two palettes are not the same stylesheet");

    /* A light palette must not contain a dark window background. */
    lg_rgb bg = {0, 0, 0};
    char hex[8] = {0};
    if (css_color_for(light, "window, .background", "background-color", &bg, hex, sizeof(hex))) {
        check(relative_luminance(bg) > 0.5, "the light palette has a light window background");
    }
    if (css_color_for(dark, "window, .background", "background-color", &bg, hex, sizeof(hex))) {
        check(relative_luminance(bg) < 0.2, "the dark palette has a dark window background");
    }
}

int main(void)
{
    printf("palette contrast\n");
    test_header_rules_exist(LG_THEME_MODE_LIGHT, "light");
    test_header_rules_exist(LG_THEME_MODE_DARK, "dark");
    test_body_contrast(LG_THEME_MODE_LIGHT, "light");
    test_body_contrast(LG_THEME_MODE_DARK, "dark");
    test_palettes_differ();

    printf("%ld checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
