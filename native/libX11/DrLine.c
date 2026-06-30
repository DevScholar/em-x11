/*
 * XDrawLine — draw a single line.
 * Upstream: libX11/src/DrLine.c
 */

#include "em_x11_internal.h"

int XDrawLine(
  Display* display, Drawable d, GC gc, int x1, int y1, int x2, int y2) {
  (void)display;
  if (!gc || gc_draw_disabled(gc))
    return 0;
  em_x11_js_draw_line(
    (Window)d, x1, y1, x2, y2, gc->foreground, gc->line_width);
  return 1;
}
