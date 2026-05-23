/*
 * XDrawRectangle — outline a rectangle.
 * Upstream: libX11/src/DrRect.c
 */
#include "DrawingPriv.h"

int XDrawRectangle(Display *display, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height) {
    (void)display;
    if (!gc || gc_draw_disabled(gc)) return 0;
    int x2 = x + (int)width;
    int y2 = y + (int)height;
    emx11_js_draw_line((Window)d, x,  y,  x2, y,  gc->foreground, gc->line_width);
    emx11_js_draw_line((Window)d, x2, y,  x2, y2, gc->foreground, gc->line_width);
    emx11_js_draw_line((Window)d, x2, y2, x,  y2, gc->foreground, gc->line_width);
    emx11_js_draw_line((Window)d, x,  y2, x,  y,  gc->foreground, gc->line_width);
    return 1;
}
