/*
 * XDrawSegments — draw unconnected line segments.
 * Upstream: libX11/src/DrSegs.c
 */

#include "em_x11_internal.h"

int XDrawSegments(
  Display* display, Drawable d, GC gc, XSegment* segments, int nsegments) {
  (void)display;
  if (!gc || gc_draw_disabled(gc) || !segments)
    return 0;
  for (int i = 0; i < nsegments; i++) {
    em_x11_js_draw_line((Window)d,
                        segments[i].x1,
                        segments[i].y1,
                        segments[i].x2,
                        segments[i].y2,
                        gc->foreground,
                        gc->line_width);
  }
  return 1;
}
