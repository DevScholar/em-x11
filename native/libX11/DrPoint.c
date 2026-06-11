/*
 * XDrawPoint — draw a single point.
 * Upstream: libX11/src/DrPoint.c
 */
#include "DrawingPriv.h"

int XDrawPoint(Display* display, Drawable d, GC gc, int x, int y) {
  (void)display;
  if (!gc || gc_draw_disabled(gc))
    return 0;
  em_x11_js_fill_rect((Window)d, x, y, 1, 1, gc->foreground);
  return 1;
}
