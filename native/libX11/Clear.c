/*
 * XClearWindow — clear entire window to its background.
 * Upstream: libX11/src/Clear.c
 */
#include "emx11_internal.h"

int XClearWindow(Display *display, Window w) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    emx11_js_clear_area(w, 0, 0, win->width, win->height);
    return 1;
}
