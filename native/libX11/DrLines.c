/*
 * XDrawLines — draw connected line segments.
 * Upstream: libX11/src/DrLines.c
 */
#include "DrawingPriv.h"

int XDrawLines(
  Display* display, Drawable d, GC gc, XPoint* points, int npoints, int mode) {
  (void)display;
  if (!gc || gc_draw_disabled(gc) || !points || npoints <= 0)
    return 0;
  int cx = points[0].x;
  int cy = points[0].y;
  for (int i = 1; i < npoints; i++) {
    int nx, ny;
    if (mode == CoordModePrevious) {
      nx = cx + points[i].x;
      ny = cy + points[i].y;
    } else {
      nx = points[i].x;
      ny = points[i].y;
    }
    em_x11_js_draw_line(
      (Window)d, cx, cy, nx, ny, gc->foreground, gc->line_width);
    cx = nx;
    cy = ny;
  }
  return 1;
}
