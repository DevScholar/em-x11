/*
 * XFillPolygon — fill a polygon.
 * Upstream: libX11/src/FillPoly.c
 */
#include "DrawingPriv.h"

int XFillPolygon(Display* display,
                 Drawable d,
                 GC gc,
                 XPoint* points,
                 int npoints,
                 int shape,
                 int mode) {
  (void)display;
  if (!gc || gc_draw_disabled(gc))
    return 0;
  int count = 0;
  int* flat = flatten_points(points, npoints, mode, &count);
  if (!flat)
    return 0;
  em_x11_js_fill_polygon(
    (Window)d, flat, count, shape, CoordModeOrigin, gc->foreground);
  free(flat);
  return 1;
}
