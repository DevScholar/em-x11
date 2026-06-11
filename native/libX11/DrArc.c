/*
 * XDrawArc — outline an arc.
 * Upstream: libX11/src/DrArc.c
 */
#include "DrawingPriv.h"

int XDrawArc(Display* display,
             Drawable d,
             GC gc,
             int x,
             int y,
             unsigned int width,
             unsigned int height,
             int angle1,
             int angle2) {
  (void)display;
  if (!gc || gc_draw_disabled(gc))
    return 0;
  em_x11_js_draw_arc((Window)d,
                     x,
                     y,
                     width,
                     height,
                     angle1,
                     angle2,
                     gc->foreground,
                     gc->line_width);
  return 1;
}
