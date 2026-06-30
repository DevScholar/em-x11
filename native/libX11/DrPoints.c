/*
 * XDrawPoints — draw multiple points.
 * Upstream: libX11/src/DrPoints.c
 */

#include "em_x11_internal.h"

int XDrawPoints(
  Display* display, Drawable d, GC gc, XPoint* points, int npoints, int mode) {
  (void)display;
  if (!gc || gc_draw_disabled(gc))
    return 0;
  int count = 0;
  int* flat = flatten_points(points, npoints, mode, &count);
  if (!flat)
    return 0;
  em_x11_js_draw_points(
    (Window)d, flat, count, CoordModeOrigin, gc->foreground);
  free(flat);
  return 1;
}
