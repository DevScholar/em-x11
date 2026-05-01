/*
 * Shared int-array layouts for EM_JS bridges that return more than one
 * value through an output buffer. The C consumers must agree with the
 * EM_JS body in bridges.c on indices, so define them once here.
 *
 * Each layout has a sentinel slot (..._PRESENT) that the JS body sets
 * to 1 on success and 0 otherwise -- the C side checks this before
 * reading the rest. C consumers MUST use the named indices, never raw
 * integer literals.
 *
 * IMPORTANT: EM_JS stringifies its body verbatim without running the C
 * preprocessor, so these macros cannot be referenced inside an EM_JS
 * body in bridges.c -- the EM_JS side has to use literal integer
 * indices and rely on a comment to point back to this header. Keep the
 * two in lockstep when changing the layout.
 *
 * If you add a slot:
 *   1. Bump the SIZE constant.
 *   2. Update both the EM_JS body in bridges.c AND every C reader.
 *   3. Mirror the change in src/host/index.ts (the TS data source).
 */

#ifndef EMX11_META_LAYOUT_H
#define EMX11_META_LAYOUT_H

/* emx11_js_get_window_attrs: 8 ints. */
#define EMX11_WIN_ATTRS_PRESENT       0
#define EMX11_WIN_ATTRS_X             1
#define EMX11_WIN_ATTRS_Y             2
#define EMX11_WIN_ATTRS_WIDTH         3
#define EMX11_WIN_ATTRS_HEIGHT        4
#define EMX11_WIN_ATTRS_MAPPED        5
#define EMX11_WIN_ATTRS_OVERRIDE_RED  6
#define EMX11_WIN_ATTRS_BORDER_WIDTH  7
#define EMX11_WIN_ATTRS_SIZE          8

/* emx11_js_get_window_abs_origin: 3 ints. */
#define EMX11_ABS_ORIGIN_PRESENT 0
#define EMX11_ABS_ORIGIN_AX      1
#define EMX11_ABS_ORIGIN_AY      2
#define EMX11_ABS_ORIGIN_SIZE    3

/* emx11_js_get_property_meta: 8 ints.
 *
 * Slot 6 (PRESENT) is the window-existence bit; slot 0 (FOUND) is the
 * property-existence bit on a known window. The two are distinct: a
 * known window with no such property returns PRESENT=1, FOUND=0. */
#define EMX11_PROP_META_FOUND        0
#define EMX11_PROP_META_TYPE         1
#define EMX11_PROP_META_FORMAT       2
#define EMX11_PROP_META_NITEMS       3
#define EMX11_PROP_META_BYTES_AFTER  4
#define EMX11_PROP_META_DATA_LEN     5
#define EMX11_PROP_META_PRESENT      6
#define EMX11_PROP_META_SIZE         8

#endif /* EMX11_META_LAYOUT_H */
