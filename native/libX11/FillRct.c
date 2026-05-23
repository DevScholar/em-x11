/*
 * XFillRectangle — fill a solid rectangle.
 * Upstream: libX11/src/FillRct.c
 */
#include "DrawingPriv.h"

int XFillRectangle(Display *display, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height) {
    (void)display;
    if (!gc || gc_draw_disabled(gc)) return 0;
    emx11_js_fill_rect((Window)d, x, y, width, height, gc->foreground);
    return 1;
}
