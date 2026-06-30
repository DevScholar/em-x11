/*
 * XFillArcs — fill multiple arcs.
 * Upstream: libX11/src/FillArcs.c
 */

#include "em_x11_internal.h"

int XFillArcs(Display* dpy, Drawable d, GC gc, XArc* arcs, int narcs) {
  if (!arcs)
    return 0;
  for (int i = 0; i < narcs; i++) {
    XFillArc(dpy,
             d,
             gc,
             arcs[i].x,
             arcs[i].y,
             arcs[i].width,
             arcs[i].height,
             arcs[i].angle1,
             arcs[i].angle2);
  }
  return 1;
}
