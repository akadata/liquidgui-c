#include "lg_theme.h"

/*
 * Palettes.
 *
 * Two things make this harder than setting a few colours.
 *
 * First, a TreeView column heading is not the treeview. It is a header strip
 * holding buttons, and styling `treeview` alone leaves the headings on whatever
 * the GTK theme chose. The earlier version styled only window/label/treeview/
 * entry, so in light mode the label rule recoloured the heading text to near
 * black while the heading background stayed dark, leaving the headings
 * unreadable.
 *
 * Second, Adwaita paints headers, tabs and buttons with background *images*
 * rather than flat colours, so a background-color set underneath a theme
 * background-image is simply hidden. Every surface recoloured here therefore
 * also sets background-image: none.
 *
 * Contrast is asserted by tests/test_theme.c: heading text is 13.6:1 in light
 * and 11.1:1 in dark, against the 4.5:1 minimum.
 */
static const char *const LG_CSS_DARK =
    "window, .background { background-color: #14161a; color: #e6e8ec; }\n"
    "label { color: #e6e8ec; }\n"
    "label.dim-label, label.subtitle { color: #9aa0aa; }\n"
    "treeview, treeview.view { background-color: #1b1e24; background-image: none; color: #e6e8ec; }\n"
    "treeview:selected, treeview:selected:focus { background-color: #2f4f77; color: #ffffff; }\n"
    "treeview header { background-color: #262b33; background-image: none; color: #e6e8ec; "
        "border-bottom: 1px solid #343a45; border-top: 1px solid #0e1013; padding: 4px 6px; }\n"
    "treeview header button { background-color: #262b33; background-image: none; box-shadow: none; "
        "color: #dfe3ea; border-radius: 0; padding: 4px 6px; font-weight: 600; }\n"
    "treeview header button:hover { background-color: #313742; color: #ffffff; }\n"
    "treeview header button label, treeview header label { color: #dfe3ea; }\n"
    "notebook > header > tabs > tab label { color: #9aa0aa; }\n"
    "notebook > header > tabs > tab:checked label { color: #e6e8ec; }\n"
    "checkbutton check, checkbutton indicator, radiobutton indicator { background-color: #454f5c; border: 1px solid #5a6472; }\n"
    "button { background-color: #262b33; background-image: none; color: #e6e8ec; "
        "border: 1px solid #363c47; border-radius: 4px; padding: 4px 10px; }\n"
    "button:hover { background-color: #313742; }\n"
    "button:checked { background-color: #2f4f77; border-color: #4d7ec4; }\n"
    "button:disabled { color: #6b7280; }\n"
    "checkbutton, radiobutton { color: #e6e8ec; background-image: none; }\n"
    "entry, spinbutton { background-color: #1b1e24; background-image: none; color: #e6e8ec; "
        "border: 1px solid #363c47; border-radius: 4px; }\n"
    "entry:focus, spinbutton:focus { border-color: #4d7ec4; }\n"
    "menubar, menu, .menu { background-color: #1b1e24; background-image: none; color: #e6e8ec; }\n"
    "menuitem:hover { background-color: #2f4f77; color: #ffffff; }\n"
    "notebook > header { background-color: #14161a; background-image: none; border-color: #262b33; }\n"
    "notebook > header > tabs > tab { color: #9aa0aa; background-color: #1c2027; "
        "background-image: none; box-shadow: none; }\n"
    "notebook > header > tabs > tab:checked { color: #e6e8ec; background-color: #262b33; "
        "background-image: none; box-shadow: none; }\n"
    "notebook > header > tabs > tab:hover { color: #e6e8ec; }\n"
    "notebook > stack > border { border-color: #262b33; }\n"
    "paned > separator { background-color: #262b33; min-width: 1px; min-height: 1px; }\n"
    "scrollbar, scrolledwindow > scrollbar { background-color: #14161a; }\n"
    "#banner-warn { background-color: #4a3a12; color: #f4d58a; }\n"
    "#banner-critical { background-color: #5c1f1f; color: #ffb3b3; }\n";

static const char *const LG_CSS_LIGHT =
    "window, .background { background-color: #f4f5f7; color: #1c1d20; }\n"
    "label { color: #1c1d20; }\n"
    "label.dim-label, label.subtitle { color: #55595f; }\n"
    "treeview, treeview.view { background-color: #ffffff; background-image: none; color: #1c1d20; }\n"
    "treeview:selected, treeview:selected:focus { background-color: #cfe0f7; color: #10233b; }\n"
    "treeview header { background-color: #e4e7ec; background-image: none; color: #1c1d20; "
        "border-bottom: 1px solid #c3c8d0; border-top: 1px solid #ffffff; padding: 4px 6px; }\n"
    "treeview header button { background-color: #e4e7ec; background-image: none; box-shadow: none; "
        "color: #1c1d20; border-radius: 0; padding: 4px 6px; font-weight: 600; }\n"
    "treeview header button:hover { background-color: #d5dae1; color: #000000; }\n"
    "treeview header button label, treeview header label { color: #1c1d20; }\n"
    "notebook > header > tabs > tab label { color: #55595f; }\n"
    "notebook > header > tabs > tab:checked label { color: #10233b; }\n"
    "checkbutton check, checkbutton indicator, radiobutton indicator { background-color: #ffffff; border: 1px solid #8a8f98; color: #10233b; }\n"
    "button { background-color: #e9ebef; background-image: none; color: #1c1d20; "
        "border: 1px solid #c3c8d0; border-radius: 4px; padding: 4px 10px; }\n"
    "button:hover { background-color: #dde0e6; }\n"
    "button:checked { background-color: #cfe0f7; border-color: #7aa5d8; }\n"
    "button:disabled { color: #8a8f98; }\n"
    "checkbutton, radiobutton { color: #1c1d20; background-image: none; }\n"
    "entry, spinbutton { background-color: #ffffff; background-image: none; color: #1c1d20; "
        "border: 1px solid #c3c8d0; border-radius: 4px; }\n"
    "entry:focus, spinbutton:focus { border-color: #4d7ec4; }\n"
    "menubar, menu, .menu { background-color: #ffffff; background-image: none; color: #1c1d20; }\n"
    "menuitem:hover { background-color: #cfe0f7; color: #10233b; }\n"
    "notebook > header { background-color: #f4f5f7; background-image: none; border-color: #d5dae1; }\n"
    "notebook > header > tabs > tab { color: #55595f; background-color: #e9ebef; "
        "background-image: none; box-shadow: none; }\n"
    "notebook > header > tabs > tab:checked { color: #10233b; background-color: #ffffff; "
        "background-image: none; box-shadow: none; }\n"
    "notebook > header > tabs > tab:hover { color: #1c1d20; }\n"
    "notebook > stack > border { border-color: #d5dae1; }\n"
    "paned > separator { background-color: #c3c8d0; min-width: 1px; min-height: 1px; }\n"
    "scrollbar, scrolledwindow > scrollbar { background-color: #f4f5f7; }\n"
    "#banner-warn { background-color: #fdf3d0; color: #5c4400; }\n"
    "#banner-critical { background-color: #f9d0d0; color: #7d1616; }\n";
const char *lg_theme_css(lg_theme_mode mode)
{
    return (mode == LG_THEME_MODE_LIGHT) ? LG_CSS_LIGHT : LG_CSS_DARK;
}
