/*
 * lg_theme - colour palettes, kept free of any GTK dependency.
 *
 * Separated from the interface so the palettes can be asserted on directly.
 * A TreeView column heading is not the treeview: it is a separate header strip
 * of buttons, each with its own label. Styling only the treeview leaves the
 * headings on whatever the GTK theme chose, which in light mode put near-black
 * text on the theme's dark heading background and made them unreadable. Having
 * the palettes as plain data lets tests/test_theme.c check the contrast rather
 * than relying on someone noticing in a screenshot.
 *
 * (C) 2026 AKADATA LIMITED - Andrew Smalley
 * Released under the MIT License.
 */

#ifndef LG_THEME_H
#define LG_THEME_H

typedef enum {
    LG_THEME_MODE_DARK = 0,
    LG_THEME_MODE_LIGHT = 1,
} lg_theme_mode;

/* The stylesheet for a palette. Never NULL. */
const char *lg_theme_css(lg_theme_mode mode);

#endif /* LG_THEME_H */
